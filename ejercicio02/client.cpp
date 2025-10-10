// client.cpp
// Ejercicio 2 - Cliente-Servidor de Micro Base de Datos con Transacciones
// Compilar: g++ -std=gnu++17 client.cpp -o client
// Ejecutar: ./client <direccion_ip_servidor> <puerto>

#include <iostream>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>      // For close, read, write
#include <cstring>       // For memset
#include <limits>        // For numeric_limits
#include <stdexcept>     // For std::runtime_error

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Uso: " << argv[0] << " <direccion_ip_servidor> <puerto>\n";
        return 1;
    }

    std::string server_ip = argv[1];
    int port = std::stoi(argv[2]);

    int sock = 0;
    struct sockaddr_in serv_addr;
    char buffer[4096] = {0}; // Increased buffer size to match server

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
    if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        std::cerr << "Connection Failed to " << server_ip << ":" << port << " - " << strerror(errno) << "\n";
        close(sock);
        return 1;
    }

    std::cout << "Connected to server " << server_ip << ":" << port << std::endl;
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
        // Clear potential error flags and ignore any remaining characters in the buffer
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
        std::cout << "Server response:\n" << buffer;
    }

    close(sock);
    std::cout << "Disconnected from server.\n";

    return 0;
}