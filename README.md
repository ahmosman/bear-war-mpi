# Bear War MPI

## Project Description

In a distant future, war breaks out between the Terran Federation and cute teddy bears, whose adorable appearance completely contradicts their dark and sadistic nature. The war is guerrilla-style, with the federation sending individual ships to battle, which return in various states of damage.

The ships compete for one of several indistinguishable docks and a random number of indistinguishable mechanics (Z), where Z is determined randomly based on battle damage.

**Processes:** N ships  
**Resources:** K docks, M mechanics

The processes operate at different speeds and may rest for periods of time. This should not block the work of other processes.

## Installation Instructions

### Prerequisites

You need Docker to run this project:

#### For Windows:
1. Download and install [Docker Desktop for Windows](https://www.docker.com/products/docker-desktop)
1. Make sure WSL2 is enabled (Windows 10/11 requirement)
1. Start Docker Desktop after installation

#### For Linux:
Install Docker by following the instructions on the [Docker website](https://docs.docker.com/engine/install/)

### Setup Instructions

1. Clone or download this repository
2. Create a `shared` directory in the project folder:
   ```bash
   mkdir shared
   ```
3. Build and start the containers:
   ```bash
   docker-compose up -d
   ```
4. Connect to any node (e.g., node1):
   ```bash
   docker exec -it mpi_node1 bash
   ```
   OR
   
   being in the project directory:
    ```bash
    docker-compose exec node1 bash
    ```

## Using MPI

### Compiling Programs

To compile a program, use the `mpi++` command. For example:

```bash
mpic++ bear_war.cpp -o bear_war
```

### Running Programs

To run an MPI program:

```bash
mpirun -np <N_ships> bear_war <K_docks> <M_mechanics>
```

Where  `<N_ships>` is the number of ships (processes), `<K_docks>` is the number of docks, and `<M_mechanics>` is the number of mechanics.

To run across multiple nodes, create a hostfile (e.g., `hosts`) in the `/app/shared` directory:

```
node1
node2
node3
node4
```

Then run with (np is the total number of processes across all nodes):

```bash
mpirun -hostfile hosts -np <N_ships> bear_war <K_docks> <M_mechanics>
```
Where  `<N_ships>` is the number of ships (processes), `<K_docks>` is the number of docks, and `<M_mechanics>` is the number of mechanics.

### Be careful
When running MPI programs on multiple nodes, ensure that every node can ssh into every other node without a password prompt. Fingerprints should be accepted. 

## Why Run From /app/shared?

The `/app/shared` directory is a volume mounted from the host to all containers. This means:

1. **File Sharing:** All nodes can access the same files
2. **Persistence:** Files remain even if containers are restarted
3. **Development:** You can edit files on your host system with your preferred editor
4. **Execution:** Compiled binaries are accessible to all nodes

This setup facilitates distributed computing where all nodes need access to the same resources.