/*
Wojna z misiami

W odległej przyszłości wybucha wojna federacji terrańskiej z puchatymi misiami, których
słitaśny wygląd kompletnie nie pasuje do ich mrocznej i sadystycznej natury. Wojna ma
charakter podjazdowy, federacja wysyła pojedyncze okręty do walki, po których wracają
zniszczone w różnym stopniu.

Okręty ubiegają się o jeden z nierozróżnialnych doków i Z nierozróżnialnych mechaników,
gdzie Z ustalane jest losowo (to zniszczenia po bitwie)
Procesy: N okretów
Zasoby: K doków, M mechaników

Procesy działają z różną prędkością, mogą wręcz przez jakiś czas odpoczywać. Nie powinno to blokować pracy innych procesów.
*/

#include <mpi.h>
#include <pthread.h>
#include <iostream>
#include <vector>
#include <queue>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <cstdio>

using namespace std;

// Stałe
#define TRUE 1
#define FALSE 0
#define DEBUG 0 // Ustaw na 1, aby włączyć debugowanie

// Typy wiadomości
#define REQUEST_DOCK 1
#define REPLY_DOCK 2
#define RELEASE_DOCK 3
#define REQUEST_MECHANICS 4
#define REPLY_MECHANICS 5
#define RELEASE_MECHANICS 6
#define FINISH 7

// Zmienne globalne
int pid, ships;
int lamport_clock = 0;
int K_docks, M_mechanics; // Liczba doków i mechaników
int needed_mechanics;     // Z - potrzebna liczba mechaników dla tego okrętu

// Zmienne do synchronizacji żądań
int my_dock_request_timestamp = -1;
int my_mechanics_request_timestamp = -1;

// Stany procesu
typedef enum
{
    InRun,
    InWantDock,
    InWantMechanics,
    InRepair,
    InFinish
} state_t;
state_t stan = InRun;

// Mutexy i zmienne synchronizacji
pthread_mutex_t stateMut = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t clockMut = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t queueMut = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t resourceCond = PTHREAD_COND_INITIALIZER;

// Liczniki ACK
int dock_ack_count = 0;
int mechanics_ack_count = 0;

// Struktura żądania
struct Request
{
    int timestamp;
    int process_id;
    int mechanics_needed; // Tylko dla żądań mechaników

    bool operator<(const Request &other) const
    {
        if (timestamp != other.timestamp)
        {
            return timestamp > other.timestamp; // Priority queue - najmniejszy timestamp na górze
        }
        return process_id > other.process_id;
    }
};

// Kolejki żądań
priority_queue<Request> dock_queue;
priority_queue<Request> mechanics_queue;

// Struktura pakietu
typedef struct
{
    int timestamp;
    int src;
    int mechanics_needed; // Dla żądań mechaników
} packet_t;

MPI_Datatype MPI_PAKIET_T;
pthread_t threadKom;

// Funkcja kolorowego drukowania
void print_color(const string &message)
{
    // Generowanie kolorów w oparciu o PID
    int r = 100 + (pid * 37) % 156; // Czerwony (zakres 100-255)
    int g = 100 + (pid * 53) % 156; // Zielony (zakres 100-255)
    int b = 100 + (pid * 67) % 156; // Niebieski (zakres 100-255)

    // Formatowanie koloru w ANSI
    char color_code[20];
    snprintf(color_code, sizeof(color_code), "\x1B[38;2;%d;%d;%dm", r, g, b);

    // Wypisanie wiadomości w kolorze
    cout << color_code << "[" << pid << "] [" << lamport_clock << "] "
         << message << "\033[0m" << endl; // \033[0m resetuje kolor
}

// Funkcje pomocnicze
void update_lamport_clock(int received_timestamp)
{
    pthread_mutex_lock(&clockMut);
    lamport_clock = max(lamport_clock, received_timestamp) + 1;
    pthread_mutex_unlock(&clockMut);
}

void increment_lamport_clock()
{
    pthread_mutex_lock(&clockMut);
    lamport_clock++;
    pthread_mutex_unlock(&clockMut);
}

int get_lamport_clock()
{
    pthread_mutex_lock(&clockMut);
    int current_clock = lamport_clock;
    pthread_mutex_unlock(&clockMut);
    return current_clock;
}

void changeState(state_t newState)
{
    pthread_mutex_lock(&stateMut);
    if (stan == InFinish)
    {
        pthread_mutex_unlock(&stateMut);
        return;
    }
    stan = newState;
    pthread_cond_broadcast(&resourceCond);
    pthread_mutex_unlock(&stateMut);
}

// Inicjalizacja typu pakietu MPI
void inicjuj_typ_pakietu()
{
    int blocklengths[3] = {1, 1, 1};
    MPI_Datatype typy[3] = {MPI_INT, MPI_INT, MPI_INT};
    MPI_Aint offsets[3];

    offsets[0] = offsetof(packet_t, timestamp);
    offsets[1] = offsetof(packet_t, src);
    offsets[2] = offsetof(packet_t, mechanics_needed);

    MPI_Type_create_struct(3, blocklengths, offsets, typy, &MPI_PAKIET_T);
    MPI_Type_commit(&MPI_PAKIET_T);
}

// Wysyłanie pakietu
void sendPacket(int timestamp, int mechanics_needed, int destination, int tag)
{
    packet_t pkt;
    pkt.timestamp = timestamp;
    pkt.src = pid;
    pkt.mechanics_needed = mechanics_needed;

    MPI_Send(&pkt, 1, MPI_PAKIET_T, destination, tag, MPI_COMM_WORLD);
}

// Sprawdzanie czy można wejść do doku (K najstarszych żądań)
bool can_enter_dock()
{
    pthread_mutex_lock(&queueMut);

    // Tworzymy kopię kolejki do analizy
    priority_queue<Request> temp_queue = dock_queue;
    vector<Request> requests;

    while (!temp_queue.empty())
    {
        requests.push_back(temp_queue.top());
        temp_queue.pop();
    }

    // Sortujemy według timestamp (rosnąco) i process_id
    sort(requests.begin(), requests.end(), [](const Request &a, const Request &b)
         {
        if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
        return a.process_id < b.process_id; });

    bool can_enter = false;
   

    for (int i = 0; i < min(K_docks, (int)requests.size()); i++)
    {
        if (requests[i].process_id == pid && requests[i].timestamp == my_dock_request_timestamp)
        {
            can_enter = true;
            break;
        }
    }

    pthread_mutex_unlock(&queueMut);
    return can_enter;
}

// Sprawdzanie czy można wejść do sekcji mechaników
bool can_enter_mechanics()
{
    pthread_mutex_lock(&queueMut);

    // Tworzymy kopię kolejki do analizy
    priority_queue<Request> temp_queue = mechanics_queue;
    vector<Request> requests;

    while (!temp_queue.empty())
    {
        requests.push_back(temp_queue.top());
        temp_queue.pop();
    }

    // Sortujemy według timestamp (rosnąco) i process_id
    sort(requests.begin(), requests.end(), [](const Request &a, const Request &b)
         {
        if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
        return a.process_id < b.process_id; });

    int used_mechanics = 0;
    bool found_myself = false;

    for (const auto &req : requests)
    {
        if (req.process_id == pid && req.timestamp == my_mechanics_request_timestamp)
        {
            found_myself = true;
            break;
        }
        used_mechanics += req.mechanics_needed;
    }

    bool can_enter = found_myself && (used_mechanics + needed_mechanics <= M_mechanics);

    pthread_mutex_unlock(&queueMut);
    return can_enter;
}

bool should_finish()
{
    pthread_mutex_lock(&stateMut);
    bool finish = (stan == InFinish);
    pthread_mutex_unlock(&stateMut);
    return finish;
}

// Wątek komunikacyjny
void *startKomWatek(void *ptr)
{
    MPI_Status status;
    packet_t pakiet;

    while (!should_finish())
    {
        MPI_Recv(&pakiet, 1, MPI_PAKIET_T, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

        update_lamport_clock(pakiet.timestamp);

        switch (status.MPI_TAG)
        {
        case REQUEST_DOCK:
        {
            if (DEBUG)
                print_color("Otrzymałem żądanie doku od okrętu " + to_string(pakiet.src) + " z timestampem " + to_string(pakiet.timestamp));

            // Dodaj żądanie do kolejki
            pthread_mutex_lock(&queueMut);
            Request req = {pakiet.timestamp, pakiet.src, 0};
            dock_queue.push(req);
            pthread_mutex_unlock(&queueMut);

            // Wyślij REPLY
            increment_lamport_clock();
            sendPacket(get_lamport_clock(), 0, pakiet.src, REPLY_DOCK);
            break;
        }

        case REPLY_DOCK:
        {
            if (DEBUG)
                print_color("Otrzymałem zgodę na dok od okrętu " + to_string(pakiet.src) + " z timestampem " + to_string(pakiet.timestamp));
            pthread_mutex_lock(&stateMut);
            dock_ack_count++;
            pthread_mutex_unlock(&stateMut);
            pthread_cond_broadcast(&resourceCond);
            break;
        }

        case RELEASE_DOCK:
        {
            if (DEBUG)
                print_color("Otrzymałem zwolnienie doku od okrętu " + to_string(pakiet.src) + " z timestampem " + to_string(pakiet.timestamp));

            // Usuń żądanie z kolejki
            pthread_mutex_lock(&queueMut);
            priority_queue<Request> new_queue;
            while (!dock_queue.empty())
            {
                Request req = dock_queue.top();
                dock_queue.pop();
                if (!(req.process_id == pakiet.src && req.timestamp == pakiet.timestamp))
                {
                    new_queue.push(req);
                }
            }
            dock_queue = new_queue;
            pthread_mutex_unlock(&queueMut);

            pthread_cond_broadcast(&resourceCond);
            break;
        }

        case REQUEST_MECHANICS:
        {
            if (DEBUG)
                print_color("Otrzymałem żądanie " + to_string(pakiet.mechanics_needed) +
                        " mechaników od okrętu " + to_string(pakiet.src) + " z timestampem " + to_string(pakiet.timestamp));

            // Dodaj żądanie do kolejki
            pthread_mutex_lock(&queueMut);
            Request req = {pakiet.timestamp, pakiet.src, pakiet.mechanics_needed};
            mechanics_queue.push(req);
            pthread_mutex_unlock(&queueMut);

            // Wyślij REPLY
            increment_lamport_clock();
            sendPacket(get_lamport_clock(), 0, pakiet.src, REPLY_MECHANICS);
            break;
        }

        case REPLY_MECHANICS:
            if (DEBUG)
                print_color("Otrzymałem zgodę na mechaników od okrętu " + to_string(pakiet.src) + " z timestampem " + to_string(pakiet.timestamp));
            pthread_mutex_lock(&stateMut);
            mechanics_ack_count++;
            pthread_mutex_unlock(&stateMut);
            pthread_cond_broadcast(&resourceCond);
            break;

        case RELEASE_MECHANICS:
        {
            if (DEBUG)
                print_color("Otrzymałem zwolnienie mechaników od okrętu " + to_string(pakiet.src) + " z timestampem " + to_string(pakiet.timestamp));

            // Usuń żądanie z kolejki
            pthread_mutex_lock(&queueMut);
            priority_queue<Request> new_queue;
            while (!mechanics_queue.empty())
            {
                Request req = mechanics_queue.top();
                mechanics_queue.pop();
                if (!(req.process_id == pakiet.src && req.timestamp == pakiet.timestamp))
                {
                    new_queue.push(req);
                }
            }
            mechanics_queue = new_queue;
            pthread_mutex_unlock(&queueMut);

            pthread_cond_broadcast(&resourceCond);
            break;
        }
        }
    }

    return NULL;
}

// Główna pętla procesu
void mainLoop()
{
    srand(pid + time(NULL));

    while (stan != InFinish)
    {

        switch (stan)
        {
        case InRun:
        {
            // Losowe decydowanie czy okręt wraca z bitwy (20% szansy)
            int perc = rand() % 100;
            if (perc < 20)
            {
                // Losowanie liczby potrzebnych mechaników (1-M)
                needed_mechanics = 1 + rand() % M_mechanics;
                print_color("Powrot z wojny, potrzebuje " + to_string(needed_mechanics) + " mechanikow");

                // Żądanie doku
                increment_lamport_clock();
                int my_timestamp = get_lamport_clock();

                // przypisanie aby porównać z zegarem requesta (był problem gdy między requestami zegar się zmienia)
                my_dock_request_timestamp = my_timestamp;

                // Dodaj własne żądanie do kolejki doku
                pthread_mutex_lock(&queueMut);
                Request dock_req = {my_timestamp, pid, 0};
                dock_queue.push(dock_req);
                pthread_mutex_unlock(&queueMut);

                // Wyślij żądanie doku do wszystkich
                pthread_mutex_lock(&stateMut); // ZABLOKUJ MUTEX
                dock_ack_count = 0;
                pthread_mutex_unlock(&stateMut);
                for (int i = 0; i < ships; i++)
                {
                    if (i != pid)
                    {
                        sendPacket(my_timestamp, 0, i, REQUEST_DOCK);
                    }
                }

                print_color("Zadam doku");
                changeState(InWantDock);
            }

            // Czekaj na sygnał zamiast sleep
            pthread_mutex_lock(&stateMut);
            struct timespec timeout;
            clock_gettime(CLOCK_REALTIME, &timeout);
            timeout.tv_sec += 1; // Czekaj max 1 sekundę

            pthread_cond_timedwait(&resourceCond, &stateMut, &timeout);
            pthread_mutex_unlock(&stateMut);
            break;
        }

        case InWantDock:
        { 
            bool got_dock = false;
            pthread_mutex_lock(&stateMut);
            while (stan == InWantDock && (dock_ack_count < ships - 1 || !can_enter_dock()))
            {
                if (DEBUG)
                    print_color("Czekam na dostęp do doku... (ACK: " + to_string(dock_ack_count) + "/" + to_string(ships - 1) + ")");
                
                pthread_cond_wait(&resourceCond, &stateMut);
            }
            if (stan == InWantDock)
            { // Sprawdzamy, czy warunek został spełniony
                got_dock = true;
                mechanics_ack_count = 0; 
            }
            pthread_mutex_unlock(&stateMut); 

            if (got_dock)
            {
                print_color("Zadokowano!");

                // Żądanie mechaników 
                increment_lamport_clock();
                int my_timestamp = get_lamport_clock();
                // przypisanie aby porównać z zegarem requesta
                my_mechanics_request_timestamp = my_timestamp;

                pthread_mutex_lock(&queueMut);
                Request mech_req = {my_timestamp, pid, needed_mechanics};
                mechanics_queue.push(mech_req);
                pthread_mutex_unlock(&queueMut);

                 // Wyślij żądanie mechaników do wszystkich
                for (int i = 0; i < ships; i++)
                {
                    if (i != pid)
                    {
                        sendPacket(my_timestamp, needed_mechanics, i, REQUEST_MECHANICS);
                    }
                }

                print_color("Zadam " + to_string(needed_mechanics) + " mechanikow");
                changeState(InWantMechanics); 
            }
            break;
        }

        case InWantMechanics:
        {
            bool got_mechanics = false; 

         
            pthread_mutex_lock(&stateMut);
            // Pętla oczekująca na dostęp do mechaników i zgody od innych
            while (stan == InWantMechanics && (mechanics_ack_count < ships - 1 || !can_enter_mechanics()))
            {
                if (DEBUG)
                    print_color("Czekam na dostęp do mechaników... (ACK: " + to_string(mechanics_ack_count) + "/" + to_string(ships - 1) + ")");
                
                pthread_cond_wait(&resourceCond, &stateMut);
            }

           
            if (stan == InWantMechanics)
            {
                got_mechanics = true;
            }

            pthread_mutex_unlock(&stateMut);
         
            // Działania wykonywane po zwolnieniu mutexu
            if (got_mechanics)
            {
                print_color("Rozpoczynam naprawe z " + to_string(needed_mechanics) + " mechanikami");

                changeState(InRepair);
            }
            break;
        }

        case InRepair:
        {
            print_color("Naprawiam okręt... (może potrwać 2-7 sekund)");
            int repair_time = 3 + rand() % 5; // 2-7 sekund
            sleep(repair_time);

            print_color("Naprawa zakonczona");
            print_color("Zwalniam dok i " + to_string(needed_mechanics) + " mechanikow");
            // Zwolnienie mechaników
            increment_lamport_clock();
            for (int i = 0; i < ships; i++)
            {
                if (i != pid)
                {
                    sendPacket(my_mechanics_request_timestamp, needed_mechanics, i, RELEASE_MECHANICS);
                }
            }

            // Zwolnienie doku
            increment_lamport_clock();
            for (int i = 0; i < ships; i++)
            {
                if (i != pid)
                {
                    sendPacket(my_dock_request_timestamp, 0, i, RELEASE_DOCK);
                }
            }


            // Usuń własne żądania z kolejek
            pthread_mutex_lock(&queueMut);
            // Czyszczenie kolejki doków
            priority_queue<Request> new_dock_queue;
            while (!dock_queue.empty())
            {
                Request req = dock_queue.top();
                dock_queue.pop();
                if (req.process_id != pid)
                {
                    new_dock_queue.push(req);
                }
            }
            dock_queue = new_dock_queue;

            // Czyszczenie kolejki mechaników
            priority_queue<Request> new_mech_queue;
            while (!mechanics_queue.empty())
            {
                Request req = mechanics_queue.top();
                mechanics_queue.pop();
                if (req.process_id != pid)
                {
                    new_mech_queue.push(req);
                }
            }
            mechanics_queue = new_mech_queue;
            pthread_mutex_unlock(&queueMut);

            print_color("Wracam do normalnej służby. Zasoby zwolnione.");
            changeState(InRun);
            break;
        }

        default:
            break;
        }

    }
}

int main(int argc, char **argv)
{

    if (argc >= 3)
    {
        K_docks = atoi(argv[1]);
        M_mechanics = atoi(argv[2]);
    }
    else
    {
        cout << "Użycie: " << argv[0] << " <K_docks> <M_mechanics>" << endl;
        exit(1);
    }

    int provided;
    MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided);

    if (provided != MPI_THREAD_MULTIPLE)
    {
        cout << "Błąd: MPI nie obsługuje wielowątkowości!" << endl;
        MPI_Finalize();
        return -1;
    }

    MPI_Comm_size(MPI_COMM_WORLD, &ships);
    MPI_Comm_rank(MPI_COMM_WORLD, &pid);

    if (pid == 0)
    {
        cout << "=== WOJNA Z MISIAMI ===" << endl;
        cout << "Okręty: " << ships << endl;
        cout << "Doki: " << K_docks << endl;
        cout << "Mechanicy: " << M_mechanics << endl;
        cout << "======================" << endl;
    }

    inicjuj_typ_pakietu();

    // Uruchomienie wątku komunikacyjnego
    pthread_create(&threadKom, NULL, startKomWatek, NULL);

    print_color("Okręt federacji terrańskiej gotowy do walki!");

    // Główna pętla
    mainLoop();

    // Sprzątanie
    changeState(InFinish);
    pthread_join(threadKom, NULL);

    pthread_mutex_destroy(&stateMut);
    pthread_mutex_destroy(&clockMut);
    pthread_mutex_destroy(&queueMut);
    pthread_cond_destroy(&resourceCond);

    MPI_Type_free(&MPI_PAKIET_T);
    MPI_Finalize();

    return 0;
}
