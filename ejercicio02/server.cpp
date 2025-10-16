// server.cpp
// Ejercicio 2 - Cliente-Servidor de Micro Base de Datos con Transacciones
// Compilar: g++ -std=gnu++17 server.cpp -o server
// Ejecutar: ./server <puerto> <ruta_csv> <N_clientes_concurrentes> <M_clientes_en_espera_app_queue>

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <algorithm> // For std::min, std::stoi
#include <cstdio>    // For rename

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>    // For close, fork, read, write, usleep
#include <arpa/inet.h> // For inet_ntoa
#include <sys/file.h>  // For flock
#include <cstring>     // For memset, strerror
#include <csignal>     // For sigaction
#include <sys/wait.h>  // For waitpid
#include <fcntl.h>     // For open, fcntl, O_NONBLOCK
#include <queue>       // For std::queue

// --- Global CSV file path ---
static std::string g_csv_path;

// --- Global counter for active child processes and limits ---
static int active_child_processes = 0;
static int max_allowed_concurrent_clients = 0; // N: Clientes atendidos por hijos
static int max_app_waiting_clients_queue = 0;  // M: Clientes en cola de espera de la aplicación

// --- Cola de clientes aceptados pero en espera de un manejador hijo ---
static std::queue<int> waiting_client_sockets; // FDs de clientes en espera

// --- Helper Functions for CSV operations ---

// Reads all lines from the CSV file
std::vector<std::string> read_csv_data(const std::string &path)
{
    std::vector<std::string> data;
    std::ifstream file(path);
    if (file.is_open())
    {
        std::string line;
        while (std::getline(file, line))
        {
            data.push_back(line);
        }
        file.close();
    }
    else
    {
        std::cerr << "Error: Could not open CSV file for reading: " << path << std::endl;
    }
    return data;
}

// Writes all data back to the CSV file (overwrites existing content)
bool write_csv_data(const std::string &path, const std::vector<std::string> &data)
{
    std::ofstream file(path, std::ios::out | std::ios::trunc);
    if (file.is_open())
    {
        for (const std::string &line : data)
        {
            file << line << "\n";
        }
        file.close();
        return true;
    }
    else
    {
        std::cerr << "Error: Could not open CSV file for writing: " << path << std::endl;
        return false;
    }
}

// --- Client Request Handler ---
void handle_client(int client_sock_fd, pid_t client_handler_pid)
{
    char buffer[4096] = {0}; // Increased buffer size for larger responses/requests
    int valread;
    bool transaction_active = false; // Flag for this specific client's transaction state

    // Each child process must open its own file descriptor to the CSV for `flock` to work correctly.
    int local_csv_fd = open(g_csv_path.c_str(), O_RDWR); // Open for read/write
    if (local_csv_fd == -1)
    {
        std::cerr << "[Handler PID " << getpid() << "] Error: Could not open CSV file for locking: " << g_csv_path << " - " << strerror(errno) << std::endl;
        std::string err_msg = "ERROR: Server internal error opening CSV file.\n";
        send(client_sock_fd, err_msg.c_str(), err_msg.length(), 0);
        close(client_sock_fd);
        _exit(1); // Child process exits
    }

    std::cout << "[Handler PID " << getpid() << "] Handling new client." << std::endl;

    while ((valread = read(client_sock_fd, buffer, sizeof(buffer) - 1)) > 0)
    {
        buffer[valread] = '\0'; // Null-terminate the received data
        std::string request(buffer);
        std::istringstream iss(request);
        std::string command;
        iss >> command;

        std::string response = "OK\n";

        if (command == "QUERY")
        {
            std::string search_term;
            // No transaction required for read-only query
            std::getline(iss, search_term);                                     // Read the rest of the line
            search_term.erase(0, search_term.find_first_not_of(" \t\n\r\f\v")); // Trim leading whitespace

            std::vector<std::string> records = read_csv_data(g_csv_path);
            std::string result = "";
            bool first_line = true;
            for (const auto &record : records)
            {
                if (first_line)
                {
                    result += record + "\n"; // Include header in query response
                    first_line = false;
                    continue;
                }
                if (record.find(search_term) != std::string::npos)
                {
                    result += record + "\n";
                }
            }
            // Condición para respuesta de "No records found" si solo se incluye el encabezado
            if (records.empty())
            {
                response = "ERROR: CSV file is empty.\n";
            }
            else if (result.length() <= records[0].length() + 1)
            { // +1 for newline, if records[0] is just the header
                response = "No records found for '" + search_term + "'.\n";
            }
            else
            {
                response = result;
            }
        }
        else if (command == "BEGIN_TRANSACTION")
        {
            if (transaction_active)
            {
                response = "ERROR: A transaction is already active for this client.\n";
            }
            else if (flock(local_csv_fd, LOCK_EX | LOCK_NB) == -1)
            { // Attempt exclusive lock
                if (errno == EWOULDBLOCK)
                {
                    response = "ERROR: Another transaction is active. Please reattempt later.\n";
                }
                else
                {
                    perror(("[Handler PID " + std::to_string(getpid()) + "] flock LOCK_EX (BEGIN_TRANSACTION)").c_str());
                    response = "ERROR: Could not acquire file lock: " + std::string(strerror(errno)) + "\n";
                }
            }
            else
            {
                transaction_active = true;
                response = "Transaction started. File locked.\n";
            }
        }
        else if (command == "COMMIT_TRANSACTION")
        {
            if (transaction_active)
            {
                flock(local_csv_fd, LOCK_UN); // Release the lock
                transaction_active = false;
                response = "Transaction committed. File unlocked.\n";
            }
            else
            {
                response = "ERROR: No active transaction to commit.\n";
            }
        }
        else if (command == "ADD")
        {
            if (!transaction_active)
            {
                response = "ERROR: ADD requires an active transaction.\n";
            }
            else
            {
                std::string new_record_data;
                std::getline(iss, new_record_data);                                         // Read the rest of the line
                new_record_data.erase(0, new_record_data.find_first_not_of(" \t\n\r\f\v")); // Trim leading whitespace

                if (!new_record_data.empty())
                {
                    std::vector<std::string> records = read_csv_data(g_csv_path);
                    if (records.empty())
                    {                                                      // If file was empty, just add header and record
                        records.push_back("ID,Nombre,Edad,Ciudad,Fuente"); // Default header
                    }
                    records.push_back(new_record_data); // Append the new record

                    if (write_csv_data(g_csv_path, records))
                    {
                        response = "Record added: " + new_record_data + "\n";
                    }
                    else
                    {
                        response = "ERROR: Failed to write to CSV file.\n";
                    }
                }
                else
                {
                    response = "ERROR: ADD command requires record data.\n";
                }
            }
        }
        else if (command == "MODIFY")
        {
            if (!transaction_active)
            {
                response = "ERROR: MODIFY requires an active transaction.\n";
            }
            else
            {
                std::string id_str, new_record_data_line;
                iss >> id_str;                                                                        // Read ID
                std::getline(iss, new_record_data_line);                                              // Read the rest as new record data
                new_record_data_line.erase(0, new_record_data_line.find_first_not_of(" \t\n\r\f\v")); // Trim leading whitespace

                if (!id_str.empty() && !new_record_data_line.empty())
                {
                    try
                    {
                        int id_to_modify = std::stoi(id_str);
                        std::vector<std::string> records = read_csv_data(g_csv_path);
                        bool found = false;
                        for (size_t i = 1; i < records.size(); ++i)
                        { // Start from 1 to skip header
                            std::istringstream record_iss(records[i]);
                            std::string current_id_field;
                            std::getline(record_iss, current_id_field, ',');
                            if (std::stoi(current_id_field) == id_to_modify)
                            {
                                records[i] = new_record_data_line; // Replace the entire line
                                found = true;
                                break;
                            }
                        }
                        if (found)
                        {
                            if (write_csv_data(g_csv_path, records))
                            {
                                response = "Record ID " + id_str + " modified to: " + new_record_data_line + "\n";
                            }
                            else
                            {
                                response = "ERROR: Failed to write to CSV file.\n";
                            }
                        }
                        else
                        {
                            response = "ERROR: Record with ID " + id_str + " not found.\n";
                        }
                    }
                    catch (const std::invalid_argument &e)
                    {
                        response = "ERROR: Invalid ID format.\n";
                    }
                    catch (const std::out_of_range &e)
                    {
                        response = "ERROR: ID out of range.\n";
                    }
                }
                else
                {
                    response = "ERROR: MODIFY command requires an ID and new record data.\n";
                }
            }
        }
        else if (command == "DELETE")
        {
            if (!transaction_active)
            {
                response = "ERROR: DELETE requires an active transaction.\n";
            }
            else
            {
                std::string id_str;
                iss >> id_str;
                if (!id_str.empty())
                {
                    try
                    {
                        int id_to_delete = std::stoi(id_str);
                        std::vector<std::string> records = read_csv_data(g_csv_path);
                        std::vector<std::string> new_records;
                        bool found = false;
                        if (!records.empty())
                        {
                            new_records.push_back(records[0]); // Keep header
                        }
                        for (size_t i = 1; i < records.size(); ++i)
                        { // Start from 1 to skip header
                            std::istringstream record_iss(records[i]);
                            std::string current_id_field;
                            std::getline(record_iss, current_id_field, ',');
                            if (std::stoi(current_id_field) == id_to_delete)
                            {
                                found = true; // This record is the one to delete
                            }
                            else
                            {
                                new_records.push_back(records[i]); // Keep other records
                            }
                        }
                        if (found)
                        {
                            if (write_csv_data(g_csv_path, new_records))
                            {
                                response = "Record ID " + id_str + " deleted.\n";
                            }
                            else
                            {
                                response = "ERROR: Failed to write to CSV file.\n";
                            }
                        }
                        else
                        {
                            response = "ERROR: Record with ID " + id_str + " not found.\n";
                        }
                    }
                    catch (const std::invalid_argument &e)
                    {
                        response = "ERROR: Invalid ID format.\n";
                    }
                    catch (const std::out_of_range &e)
                    {
                        response = "ERROR: ID out of range.\n";
                    }
                }
                else
                {
                    response = "ERROR: DELETE command requires an ID.\n";
                }
            }
        }
        else
        {
            response = "ERROR: Unknown command '" + command + "'.\nAvailable commands: QUERY <term>, BEGIN_TRANSACTION, COMMIT_TRANSACTION, ADD <data>, MODIFY <id> <data>, DELETE <id>, EXIT.\n";
        }
        send(client_sock_fd, response.c_str(), response.length(), 0);
    }

    // Client disconnected or read error
    if (transaction_active)
    {
        flock(local_csv_fd, LOCK_UN); // Release lock if client disconnected during transaction
        std::cerr << "[Handler PID " << getpid() << "] WARNING: Client disconnected during an active transaction. Lock released.\n";
    }
    close(local_csv_fd); // Close the file descriptor opened by this child
    close(client_sock_fd);
    std::cout << "[Handler PID " << getpid() << "] Client disconnected. Exiting child process." << std::endl;
    _exit(0); // Child process exits
}

// Signal handler for SIGCHLD to reap zombie processes and decrement counter
void sigchld_handler(int sig)
{
    int status;
    pid_t pid;
    // WNOHANG makes waitpid non-blocking, so it doesn't wait if no child has exited
    // Loop to reap all exited children, not just one
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        active_child_processes--; // Decrement the global counter
        std::cout << "[Parent PID " << getpid() << "] Child PID " << pid << " exited. Active children: " << active_child_processes << std::endl;
    }
}

int main(int argc, char *argv[])
{
    // AHORA se esperan 5 argumentos: <puerto> <ruta_csv> <N_concurrentes> <M_app_queue> <backlog_listen_kernel>
    if (argc != 5)
    {
        std::cerr << "Uso: " << argv[0] << " <puerto> <ruta_csv> <N_clientes_concurrentes> <M_clientes_en_espera_app_queue>\n";
        std::cerr << "   <N_clientes_concurrentes> (N) es el número máximo de clientes que el servidor manejará a la vez (procesos hijos).\n";
        std::cerr << "   <M_clientes_en_espera_app_queue> (M) es el tamaño máximo de la cola de espera interna de la aplicación.\n";
        std::cerr << "   (El backlog del listen() se establecerá internamente para manejar conexiones entrantes).\n";
        return 1;
    }

    int port = std::stoi(argv[1]);
    g_csv_path = argv[2];
    max_allowed_concurrent_clients = std::stoi(argv[3]); // N
    max_app_waiting_clients_queue = std::stoi(argv[4]);  // M (cola de la aplicación)

    // El backlog real para listen() puede ser M o un poco más, dependiendo de la implementación del SO.
    // Usaremos max_app_waiting_clients_queue para el listen, ya que es el límite que el usuario especificó para "espera".
    // El kernel puede redondearlo o usar un valor ligeramente diferente.
    int kernel_listen_backlog = max_app_waiting_clients_queue;

    // --- LÍNEAS DE DEPURACIÓN AÑADIDAS ---
    std::cout << "[DEBUG] Server (PID " << getpid() << ") started." << std::endl;
    std::cout << "[DEBUG] Initial active_child_processes: " << active_child_processes << std::endl;
    std::cout << "[DEBUG] Max allowed concurrent clients (N): " << max_allowed_concurrent_clients << std::endl;
    std::cout << "[DEBUG] Max waiting clients in app queue (M): " << max_app_waiting_clients_queue << std::endl;
    std::cout << "[DEBUG] Kernel listen() backlog set to: " << kernel_listen_backlog << std::endl;
    // ------------------------------------

    // Set up SIGCHLD handler to prevent zombie processes and update counter
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1)
    {
        perror("sigaction for SIGCHLD");
        return 1;
    }

    // --- Server Socket Setup ---
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    socklen_t addrlen = sizeof(address);

    // Create socket file descriptor
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0)
    {
        perror("socket failed");
        return 1;
    }

    // Forcefully attach socket to the port (prevents "Address already in use" after crash)
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt)))
    {
        perror("setsockopt");
        close(server_fd);
        return 1;
    }
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY; // Listen on all available network interfaces
    address.sin_port = htons(port);

    // Bind the socket to the specified port
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        perror("bind failed");
        close(server_fd);
        return 1;
    }

    // Usar kernel_listen_backlog para el backlog de listen()
    if (listen(server_fd, kernel_listen_backlog) < 0)
    {
        perror("listen");
        close(server_fd);
        return 1;
    }

    std::cout << "Server listening on port " << port << " for CSV file: " << g_csv_path << std::endl;
    std::cout << "Maximum concurrent clients allowed (N): " << max_allowed_concurrent_clients << std::endl;
    std::cout << "Maximum clients in application waiting queue (M): " << max_app_waiting_clients_queue << std::endl;
    std::cout << "Waiting for client connections...\n";

    // Establecer el socket de escucha en modo no bloqueante
    // Esto es crucial para poder intentar accept() y luego revisar la cola sin bloquearse.
    int flags = fcntl(server_fd, F_GETFL, 0);
    if (flags == -1)
    {
        perror("fcntl F_GETFL");
        close(server_fd);
        return 1;
    }
    if (fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        perror("fcntl F_SETFL O_NONBLOCK");
        close(server_fd);
        return 1;
    }

    while (true)
    {
        // --- Paso 1: Intentar aceptar nuevas conexiones entrantes (no bloqueante) ---
        // Se hace de forma no bloqueante para poder procesar la cola de espera y los hijos salientes.
        if ((new_socket = accept(server_fd, (struct sockaddr *)&address, &addrlen)) >= 0)
        {
            std::cout << "[Parent PID " << getpid() << "] New client accepted from " << inet_ntoa(address.sin_addr) << ":" << ntohs(address.sin_port) << std::endl;

            if (active_child_processes < max_allowed_concurrent_clients)
            {
                // N clientes concurrentes NO alcanzado: asignar un manejador hijo inmediatamente
                // Enviar un mensaje de "listo" antes de forkar, para que el cliente sepa que será atendido.
                std::string ready_msg = "SERVER: Connected and ready to process commands.\n";
                send(new_socket, ready_msg.c_str(), ready_msg.length(), 0);

                pid_t pid = fork();
                if (pid < 0)
                {
                    perror("fork failed");
                    std::string err_msg = "ERROR: Server could not fork a new process to handle client.\n";
                    send(new_socket, err_msg.c_str(), err_msg.length(), 0);
                    close(new_socket); // Cerrar el socket para el padre en caso de fallo de fork
                }
                else if (pid == 0)
                {                     // Proceso hijo
                    close(server_fd); // El hijo cierra el socket de escucha
                    handle_client(new_socket, getpid());
                    // _exit(0) se llama dentro de handle_client
                }
                else
                {                             // Proceso padre
                    close(new_socket);        // El padre cierra el socket del nuevo cliente (el hijo lo maneja)
                    active_child_processes++; // Incrementar contador de hijos activos
                    std::cout << "[Parent PID " << getpid() << "] Forked child PID " << pid << ". Active children: " << active_child_processes << std::endl;
                }
            }
            else if (waiting_client_sockets.size() < max_app_waiting_clients_queue)
            {
                // N clientes concurrentes YA alcanzado, pero la cola de la aplicación NO está llena.
                // Enviar mensaje de espera y encolar el socket.
                std::string wait_msg = "SERVER: Max concurrent clients reached. You are in waiting queue. Please wait...\n";
                send(new_socket, wait_msg.c_str(), wait_msg.length(), 0);
                waiting_client_sockets.push(new_socket);
                std::cout << "[Parent PID " << getpid() << "] Client " << inet_ntoa(address.sin_addr) << ":" << ntohs(address.sin_port) << " enqueued. Waiting queue size: " << waiting_client_sockets.size() << std::endl;
            }
            else
            {
                // N clientes concurrentes YA alcanzado Y la cola de la aplicación TAMBIÉN está llena.
                // Rechazar la conexión explícitamente.
                std::string refused_msg = "SERVER: Connection refused. Server's active client limit reached and waiting queue is full. Please try again later.\n";
                send(new_socket, refused_msg.c_str(), refused_msg.length(), 0);
                std::cout << "[Parent PID " << getpid() << "] Client " << inet_ntoa(address.sin_addr) << ":" << ntohs(address.sin_port) << " refused (queue full).\n";
                close(new_socket); // Es crucial cerrar el socket aquí.
            }
        }
        else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            // Un error real en accept que no sea "no hay conexiones pendientes" o "interrumpido por señal"
            perror("accept");
            // Considerar cerrar el servidor en caso de errores críticos de accept
        }

        // --- Paso 2: Procesar la cola de clientes en espera si hay slots disponibles (N no alcanzado) ---
        // Revisamos si un hijo terminó y liberó un slot, y si hay clientes en la cola de espera.
        while (!waiting_client_sockets.empty() && active_child_processes < max_allowed_concurrent_clients)
        {
            int client_sock_from_queue = waiting_client_sockets.front();
            waiting_client_sockets.pop();

            std::cout << "[Parent PID " << getpid() << "] Dequeuing client from waiting list. Queue size: " << waiting_client_sockets.size() << std::endl;

            // Enviar un mensaje de "es tu turno" antes de forkar el manejador
            std::string turn_msg = "SERVER: Your turn! Processing your request now.\n";
            send(client_sock_from_queue, turn_msg.c_str(), turn_msg.length(), 0);

            pid_t pid = fork();
            if (pid < 0)
            {
                perror("fork failed for queued client");
                std::string err_msg = "ERROR: Server could not fork a new process for queued client.\n";
                send(client_sock_from_queue, err_msg.c_str(), err_msg.length(), 0);
                close(client_sock_from_queue); // Cerrar el socket si no se puede forkar
            }
            else if (pid == 0)
            {                     // Proceso hijo
                close(server_fd); // El hijo cierra el socket de escucha
                handle_client(client_sock_from_queue, getpid());
            }
            else
            {                                  // Proceso padre
                close(client_sock_from_queue); // El padre cierra el socket (el hijo lo maneja)
                active_child_processes++;      // Incrementar contador de hijos activos
                std::cout << "[Parent PID " << getpid() << "] Forked child PID " << pid << " for queued client. Active children: " << active_child_processes << std::endl;
            }
        }

        usleep(50000); // Pequeña pausa para no consumir CPU inútilmente si no hay actividad
    }

    close(server_fd);
    return 0;
}