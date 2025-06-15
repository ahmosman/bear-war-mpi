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
#include <iostream>
#include <vector>
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <unistd.h>
#include <queue>

using namespace std;

const int TAG_REQUEST_DOCK = 1;
const int TAG_REPLY_DOCK = 2;
const int TAG_RELEASE_DOCK = 3;
const int TAG_REQUEST_MECHANICS = 4;
const int TAG_REPLY_MECHANICS = 5;
const int TAG_RELEASE_MECHANICS = 6;

struct Request
{
    int timestamp;
    int pid;
    int mechanics_needed; // tylko dla żądań mechaników

    bool operator<(const Request &other) const
    {
        if (timestamp != other.timestamp)
        {
            return timestamp > other.timestamp; // dla priority_queue (min-heap)
        }
        return pid > other.pid;
    }
};

struct Message
{
    int timestamp;
    int pid;
    int mechanics_needed;
    int tag;
};

int lamport_clock = 0;
int N, pid;
const int K = 4; // liczba doków
const int M = 20; // liczba mechaników
int Z = 0;       // liczba potrzebnych mechaników

// Stany procesu
bool want_dock = false;
bool in_dock = false;
bool want_mechanics = false;
bool in_repair = false;

// Znaczniki czasowe własnych żądań
int dock_request_timestamp = 0;
int mechanics_request_timestamp = 0;

// Liczniki otrzymanych odpowiedzi
int dock_replies_received = 0;
int mechanics_replies_received = 0;

// Kolejki żądań (uporządkowane według timestampów Lamporta)
priority_queue<Request> dock_queue;
priority_queue<Request> mechanics_queue;

// Listy procesów oczekujących na REPLY (odłożone odpowiedzi)
vector<int> pending_dock_replies;
vector<int> pending_mechanics_replies;

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

void update_clock(int received_timestamp)
{
    lamport_clock = max(lamport_clock, received_timestamp) + 1;
}

void send_message(int dest, int tag, int mechanics = 0)
{
    lamport_clock++;
    Message msg = {lamport_clock, pid, mechanics, tag};
    MPI_Send(&msg, sizeof(msg), MPI_BYTE, dest, tag, MPI_COMM_WORLD);
}

void broadcast_message(int tag, int mechanics = 0)
{
    lamport_clock++;
    Message msg = {lamport_clock, pid, mechanics, tag};
    for (int i = 0; i < N; i++)
    {
        MPI_Send(&msg, sizeof(msg), MPI_BYTE, i, tag, MPI_COMM_WORLD);
    }
}

// Sprawdza czy proces może wejść do doku (wśród K najstarszych żądań)
bool can_enter_dock()
{
    vector<Request> requests;
    priority_queue<Request> temp_queue = dock_queue;

    while (!temp_queue.empty())
    {
        requests.push_back(temp_queue.top());
        temp_queue.pop();
    }

    // Sortowanie żądań według timestampów Lamporta i PID
    sort(requests.begin(), requests.end(), [](const Request &a, const Request &b)
         {
        if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
        return a.pid < b.pid; });

    for (int i = 0; i < min(K, (int)requests.size()); i++)
    {
        if (requests[i].pid == pid && requests[i].timestamp == dock_request_timestamp)
        {
            return true;
        }
    }

    return false;
}

// Sprawdza czy proces może wejść do sekcji mechaników 
bool can_enter_mechanics()
{
    vector<Request> requests;
    priority_queue<Request> temp_queue = mechanics_queue;

    while (!temp_queue.empty())
    {
        requests.push_back(temp_queue.top());
        temp_queue.pop();
    }

    // Sortowanie żądań według znacznika czasu Lamporta i PID
    sort(requests.begin(), requests.end(), [](const Request &a, const Request &b)
         {
        if (a.timestamp != b.timestamp) return a.timestamp < b.timestamp;
        return a.pid < b.pid; });

    int mechanics_used = 0;
    for (const auto &req : requests)
    {
        if (req.timestamp < mechanics_request_timestamp ||
            (req.timestamp == mechanics_request_timestamp && req.pid < pid))
        {
            mechanics_used += req.mechanics_needed;
        }
        else if (req.pid == pid && req.timestamp == mechanics_request_timestamp)
        {
            return (mechanics_used + Z) <= M;
        }
    }
    return false;
}

void handle_request_dock(const Message &msg)
{
    update_clock(msg.timestamp);

    // Dodaj żądanie do kolejki
    Request req = {msg.timestamp, msg.pid, 0};
    dock_queue.push(req);

    // Wyślij REPLY jeśli nie konkurujemy lub mamy niższy priorytet
    if (!want_dock ||
        (msg.timestamp < dock_request_timestamp) ||
        (msg.timestamp == dock_request_timestamp && msg.pid < pid))
    {
        send_message(msg.pid, TAG_REPLY_DOCK);
    }
    else
    {
        pending_dock_replies.push_back(msg.pid); // odłóż REPLY bo konkurujemy o dok
    }
}

void handle_request_mechanics(const Message &msg)
{
    update_clock(msg.timestamp);

    // Dodaj żądanie do kolejki
    Request req = {msg.timestamp, msg.pid, msg.mechanics_needed};
    mechanics_queue.push(req);

    // Wyślij REPLY jeśli nie konkurujemy lub mamy niższy priorytet
    if (!want_mechanics ||
        (msg.timestamp < mechanics_request_timestamp) ||
        (msg.timestamp == mechanics_request_timestamp && msg.pid < pid))
    {
        send_message(msg.pid, TAG_REPLY_MECHANICS);
    }
    else
    {
        pending_mechanics_replies.push_back(msg.pid); // odłóż REPLY bo konkurujemy o mechaników
    }
}

void handle_release_dock(const Message &msg)
{
    update_clock(msg.timestamp);

    // Usuń żądanie z kolejki
    priority_queue<Request> new_queue;
    while (!dock_queue.empty())
    {
        Request req = dock_queue.top();
        dock_queue.pop();
        if (req.pid != msg.pid)
        {
            new_queue.push(req);
        }
    }
    dock_queue = new_queue;

    // Wyślij odłożone REPLY
    for (int dest_pid : pending_dock_replies)
    {
        send_message(dest_pid, TAG_REPLY_DOCK);
    }
    pending_dock_replies.clear();
}

void handle_release_mechanics(const Message &msg)
{
    update_clock(msg.timestamp);

    // Usuń żądanie z kolejki
    priority_queue<Request> new_queue;
    while (!mechanics_queue.empty())
    {
        Request req = mechanics_queue.top();
        mechanics_queue.pop();
        if (req.pid != msg.pid)
        {
            new_queue.push(req);
        }
    }
    mechanics_queue = new_queue;

    // Wyślij odłożone REPLY
    for (int dest_pid : pending_mechanics_replies)
    {
        send_message(dest_pid, TAG_REPLY_MECHANICS);
    }
    pending_mechanics_replies.clear();
}

void request_dock()
{
    want_dock = true;
    dock_request_timestamp = lamport_clock + 1;
    dock_replies_received = 0;

    // Dodaj własne żądanie do kolejki
    Request own_req = {dock_request_timestamp, pid, 0};
    dock_queue.push(own_req);

    print_color("Zadam doku");
    broadcast_message(TAG_REQUEST_DOCK);
}

void request_mechanics()
{
    want_mechanics = true;
    mechanics_request_timestamp = lamport_clock + 1;
    mechanics_replies_received = 0;

    // Dodaj własne żądanie do kolejki
    Request own_req = {mechanics_request_timestamp, pid, Z};
    mechanics_queue.push(own_req);

    print_color("Zadam " + to_string(Z) + " mechanikow");
    broadcast_message(TAG_REQUEST_MECHANICS, Z);
}

void release_resources()
{
    in_dock = false;
    in_repair = false;
    want_dock = false;
    want_mechanics = false;

    print_color("Zwalniam dok i mechanikow");

    // Usuń własne żądania z kolejek
    priority_queue<Request> new_dock_queue, new_mechanics_queue;

    while (!dock_queue.empty())
    {
        Request req = dock_queue.top();
        dock_queue.pop();
        if (!(req.pid == pid && req.timestamp == dock_request_timestamp))
        {
            new_dock_queue.push(req);
        }
    }
    dock_queue = new_dock_queue;

    while (!mechanics_queue.empty())
    {
        Request req = mechanics_queue.top();
        mechanics_queue.pop();
        if (!(req.pid == pid && req.timestamp == mechanics_request_timestamp))
        {
            new_mechanics_queue.push(req);
        }
    }
    mechanics_queue = new_mechanics_queue;

    // Wyślij komunikaty zwolnienia
    broadcast_message(TAG_RELEASE_DOCK);
    broadcast_message(TAG_RELEASE_MECHANICS);
}

int main(int argc, char **argv)
{
    MPI_Init(&argc, &argv);
    MPI_Comm_size(MPI_COMM_WORLD, &N);
    MPI_Comm_rank(MPI_COMM_WORLD, &pid);

    srand(time(NULL) + pid);
    print_color("Start programu");

    MPI_Status status;
    Message msg;
    int flag;
    int iterations_without_progress = 0;
    const int MAX_ITERATIONS = 10000;

    while (true)
    {
        bool made_progress = false;

        // Losowo decyduj o powrocie z wojny
        if (!want_dock && !in_dock && !want_mechanics && !in_repair)
        {
            if (rand() % 100 < 20)
            { // 20% szansy na powrót z wojny
                Z = 1 + rand() % M;
                print_color("Powrot z wojny, potrzebuje " + to_string(Z) + " mechanikow");
                request_dock(); // żądaj doku
                made_progress = true;
            }
        }

        // Sprawdz czy mozna wejsc do doku
        if (want_dock && !in_dock && dock_replies_received >= (N - 1) && can_enter_dock())
        {
            in_dock = true;
            print_color("Zadokowano!");
            request_mechanics(); // żądaj mechaników
            made_progress = true;
        }

        // Sprawdz czy mozna rozpoczac naprawe
        if (in_dock && want_mechanics && !in_repair &&
            mechanics_replies_received >= (N - 1) && can_enter_mechanics())
        {
            in_repair = true;
            print_color("Rozpoczynam naprawe z " + to_string(Z) + " mechanikami");

            // Symulacja naprawy
            usleep((500000 + rand() % 1000001)); // sleep pomiędzy 0.5s i 1.5s

            print_color("Naprawa zakonczona");
            release_resources();
            made_progress = true;
        }

     // Obsluga wiadomosci
        MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &flag, &status);
        if (flag)
        {
            MPI_Recv(&msg, sizeof(msg), MPI_BYTE, MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &status);

            switch (status.MPI_TAG)
            {
            case TAG_REQUEST_DOCK:
                handle_request_dock(msg);
                break;
            case TAG_REQUEST_MECHANICS:
                handle_request_mechanics(msg);
                break;
            case TAG_RELEASE_DOCK:
                handle_release_dock(msg);
                break;
            case TAG_RELEASE_MECHANICS:
                handle_release_mechanics(msg);
                break;
            case TAG_REPLY_DOCK:
                update_clock(msg.timestamp);
                dock_replies_received++;
                break;
            case TAG_REPLY_MECHANICS:
                update_clock(msg.timestamp);
                mechanics_replies_received++;
                break;
            }
        }



        // Wykrywanie deadlocka
        if (!made_progress)
        {
            iterations_without_progress++;
            if (iterations_without_progress > MAX_ITERATIONS)
            {
                print_color("DEADLOCK DETECTED - brak postępu przez " + to_string(MAX_ITERATIONS) + " iteracji");

                // Debug info
                if (want_dock && !in_dock)
                {
                    print_color("DEBUG: Czekam na dok - otrzymano " + to_string(dock_replies_received) + "/" + to_string(N - 1) + " odpowiedzi");
                }
                if (want_mechanics && !in_repair)
                {
                    print_color("DEBUG: Czekam na mechaników - otrzymano " + to_string(mechanics_replies_received) + "/" + to_string(N - 1) + " odpowiedzi");
                }
                break;
            }
        }
        else
        {
            iterations_without_progress = 0;
        }

        // Krótki sleep - unikanie aktywnego oczekiwania
        usleep(1000); // 1ms
    }

    MPI_Finalize();
    return 0;
}