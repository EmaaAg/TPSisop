// client.cpp
// Ejercicio 2 - Cliente-Servidor de Micro Base de Datos con Transacciones
// Compilar: g++ -std=gnu++17 client.cpp -o client
// Ejecutar: ./client <direccion_ip_servidor> <puerto>

#include <iostream>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>      // For close, read, write, usleep
#include <cstring>       // For memset, strerror
#include <limits>        // For numeric_limits
#include <stdexcept>     // For std::runtime_error
#include <cerrno>        // Para errno
#include <vector>        // Para usar std::vector (no estrictamente necesario aquí, pero buena práctica)

// Función para verificar si un mensaje del servidor indica que está "listo" para comandos
bool is_server_ready_message(const std::string& msg) {
    return msg.find("Connected and ready to process commands") != std::string::npos ||
           msg.find("Your turn! Processing your request now") != std::string::npos;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Uso: " << argv[0] << " <direccion_ip_servidor> <puerto>\n";
        return 1;
    }

    std::string server_ip = argv[1];
    int port = std::stoi(argv[2]);

    int sock = 0;
    struct sockaddr_in serv_addr;
    char buffer[4096] = {0};

    // Create socket
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        std::cerr << "Socket creation error\n";
        return 1;
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    // Convert IPv4 and IPv6 addresses from text to binary form
    if (inet_pton(AF_INET, server_ip.c_str(), &serv_addr.sin_addr) <= 0) {
        std::cerr << "Invalid address/ Address not supported\n";
        return 1;
    }

    // Connect to the server
    std::cout << "Attempting to connect to " << server_ip << ":" << port << "...\n";
    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "Connection Failed to " << server_ip << ":" << port << ". ";
        if (errno == ECONNREFUSED) {
            std::cerr << "Error: Connection refused. This usually means:\n";
            std::cerr << "  - The server is not running on the specified port.\n";
            std::cerr << "  - The server's waiting queue (M backlog) is full.\n";
            std::cerr << "  - Or the server's application queue (M) is full, causing explicit refusal.\n";
        } else if (errno == ETIMEDOUT) {
            std::cerr << "Error: Connection timed out. The server might be too busy or unreachable.\n";
        } else {
            std::cerr << "Error: " << strerror(errno) << "\n";
        }
        close(sock);
        return 1;
    }

    std::cout << "Connected to server " << server_ip << ":" << port << std::endl;

    std::string server_message_content;
    bool client_is_ready_to_send_commands = false;

    // Bucle para leer todos los mensajes iniciales del servidor hasta que esté "listo" o se desconecte
    while (!client_is_ready_to_send_commands) {
        memset(buffer, 0, sizeof(buffer)); // Limpiar buffer antes de leer
        int valread_initial = read(sock, buffer, sizeof(buffer) - 1);
        if (valread_initial > 0) {
            buffer[valread_initial] = '\0'; // Asegurar terminación nula
            server_message_content = buffer;
            std::cout << "Server message: " << server_message_content; // El servidor ya debe añadir '\n'

            if (is_server_ready_message(server_message_content)) {
                client_is_ready_to_send_commands = true;
            } else if (server_message_content.find("Connection refused") != std::string::npos) {
                // El servidor ha rechazado explícitamente este cliente (cola de la app llena)
                close(sock);
                std::cout << "Disconnected from server due to server refusal.\n";
                return 1; // Salir del cliente
            }
            // Si es un mensaje de "waiting queue", se imprime y el bucle continúa para esperar "Your turn!"
            // El usuario no verá el prompt hasta que client_is_ready_to_send_commands sea true.
        } else if (valread_initial == 0) {
            std::cerr << "Server disconnected immediately after connection.\n";
            close(sock);
            return 1;
        } else if (valread_initial == -1) {
            // Un error de lectura aquí (que no sea EAGAIN/EWOULDBLOCK si fuera no-bloqueante)
            // indica un problema grave, o si el servidor cierra la conexión.
            std::cerr << "Error reading initial message from server: " << strerror(errno) << "\n";
            close(sock);
            return 1;
        }
        // Pequeña pausa si no se ha recibido un mensaje de "listo" para evitar busy-waiting
        if (!client_is_ready_to_send_commands) {
            usleep(100000); // Pausa de 100ms
        }
    }

    std::cout << "Available commands:\n";
    std::cout << "  QUERY <term>           (e.g., QUERY Ana, QUERY Cordoba)\n";
    std::cout << "  BEGIN_TRANSACTION      (Starts an exclusive transaction)\n";
    std::cout << "  COMMIT_TRANSACTION     (Ends the active transaction)\n";
    std::cout << "  ADD <ID>,<Nombre>,<Edad>,<Ciudad>,<Fuente> (e.g., ADD 5,Pedro,35,Mendoza,Gen3)\n";
    std::cout << "  MODIFY <ID> <ID>,<Nombre>,<Edad>,<Ciudad>,<Fuente> (e.g., MODIFY 1 1,Ana,26,Buenos Aires,Gen1_new)\n";
    std::cout << "  DELETE <ID>            (e.g., DELETE 2)\n";
    std::cout << "  EXIT                   (Disconnects from server)\n";
    std::cout << "--------------------------------------------------------------------------------\n";


    std::string command_line;
    while (true) {
        std::cout << "\n> ";
        std::cin.clear();
        std::getline(std::cin, command_line);

        if (command_line == "EXIT") {
            break;
        }

        if (command_line.empty()) {
            continue;
        }

        // Send the command to the server
        if (send(sock, command_line.c_str(), command_line.length(), 0) == -1) {
            std::cerr << "Error sending data: " << strerror(errno) << "\n";
            break;
        }

        // Read response from server
        memset(buffer, 0, sizeof(buffer)); // Clear buffer before reading
        int valread = read(sock, buffer, sizeof(buffer) - 1);
        if (valread == 0) {
            std::cerr << "Server disconnected.\n";
            break;
        } else if (valread == -1) {
            std::cerr << "Error reading from server: " << strerror(errno) << "\n";
            break;
        }
        buffer[valread] = '\0'; // Asegurar terminación nula
        std::cout << "Server response:\n" << buffer;
    }

    close(sock);
    std::cout << "Disconnected from server.\n";

    return 0;
}