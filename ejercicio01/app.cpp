// genCSV.cpp
// Ejercicio 1 - Generador de Datos de Prueba con Procesos y Memoria Compartida
// Compilar: g++ -std=gnu++17 genCSV.cpp -o genCSV
// Ejecutar: ./genCSV <N_generadores> <total_registros> <salida.csv>

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <csignal>
#include <cerrno> // Para EAGAIN
#include <ctime>
#include <cstdlib>
#include <algorithm> // Para std::min

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace std;

// ------------------------------- IPC Keys -----------------------------------
#define SHM_KEY 0x4B1D1234
#define SEM_KEY 0x4B1D5678

// ----------------------------- Semáforos (SysV) ------------------------------
union semun {
    int val;
    struct semid_ds* buf;
    unsigned short* array;
};

static int semid = -1;

// semáforos dentro del set:
enum { SEM_MUTEX = 0, SEM_FULL_SLOT = 1, SEM_EMPTY_SLOT = 2 }; // Renombrado SEM_ITEMS a SEM_FULL_SLOT, añadido SEM_EMPTY_SLOT
#define SEM_COUNT 3 // Total de semáforos en el conjunto

static void sem_wait_idx(int semid, int idx) {
    sembuf op{static_cast<unsigned short>(idx), -1, 0};
    if (semop(semid, &op, 1) == -1) {
        perror("semop wait");
        _exit(1);
    }
}
static void sem_signal_idx(int semid, int idx) {
    sembuf op{static_cast<unsigned short>(idx), +1, 0};
    if (semop(semid, &op, 1) == -1) {
        perror("semop signal");
        _exit(1);
    }
}

// ----------------------------- Memoria Compartida ----------------------------
struct SharedData {
    // Control global
    int next_id;           // siguiente ID a asignar (1..total_registros)
    int total_registros;   // total a generar
    int total_escritos;    // contador de registros que el coordinador ya volcó al CSV
    bool terminar;         // bandera de finalización global
    int generadoresActivos;  // contador de procesos hijos activos

    // Slot de intercambio productor->consumidor
    // bool nuevoRegistro;    // <-- ELIMINADO: ahora gestionado por SEM_EMPTY_SLOT/SEM_FULL_SLOT
    int  id_publicado;              // ID del registro publicado
    char registro[512];             // línea CSV parcial (sin salto de línea)

    // Padding opcional
    char _pad[64];
};

static int shmid = -1;
static SharedData* shm = nullptr;

// ----------------------------- Limpieza Global -------------------------------
static void limpiarRecursos(bool desdeSignal = false) {
    if (shm) {
        shmdt(shm);
        shm = nullptr;
    }
    if (shmid != -1) {
        shmctl(shmid, IPC_RMID, nullptr);
        shmid = -1;
    }
    if (semid != -1) {
        semctl(semid, 0, IPC_RMID);
        semid = -1;
    }
    if (desdeSignal) _exit(0);
}

static void sigint_handler(int) {
    // Marcar terminar si existe SHM y limpiar
    if (shm) {
        // Proteger acceso mínimo
        // No usamos semáforo aquí para evitar deadlock por señales; marcamos y dejamos que salgan.
        shm->terminar = true;
    }
    limpiarRecursos(true);
}

// -------------------------- Generación de datos ------------------------------
static string generarRegistroAleatorio(int id, int idHijo) {
    // Campos de ejemplo: ID,Nombre,Edad,Ciudad,Fuente
    static const char* nombres[] = {
        "Ana","Luis","Mica","Tomas","Sofia","Lucas","Valen","Agus","Cesar","Lauti"
    };
    static const char* ciudades[] = {
        "Buenos Aires","Cordoba","Rosario","La Plata","Salta","Mendoza","Mar del Plata"
    };

    string nombre = nombres[rand() % (sizeof(nombres)/sizeof(nombres[0]))];
    int    edad   = 18 + (rand() % 61); // 18..78
    string ciudad = ciudades[rand() % (sizeof(ciudades)/sizeof(ciudades[0]))];

    // Formato CSV: ID,Nombre,Edad,Ciudad,Fuente
    string linea  = to_string(id) + "," + nombre + "," + to_string(edad) + "," + ciudad
                  + ",Gen" + to_string(idHijo);
    return linea;
}

// --------------------------- Proceso Generador -------------------------------
static void procesoGenerador(int idHijo) {
    srand(static_cast<unsigned>(time(nullptr)) ^ (getpid() << 16) ^ (idHijo * 1337));

    while (true) {
        // Bloque de IDs: necesita el mutex global
        sem_wait_idx(semid, SEM_MUTEX);

        if (shm->terminar) {
            sem_signal_idx(semid, SEM_MUTEX);
            break;
        }

        int start = shm->next_id;
        int remain = shm->total_registros - shm->next_id + 1;
        if (remain <= 0) {
            sem_signal_idx(semid, SEM_MUTEX);
            break;
        }

        int block = std::min(10, remain); // Usar std::min
        shm->next_id += block;  // reservar los 10 IDs
        sem_signal_idx(semid, SEM_MUTEX);

        // Generar y publicar cada ID del bloque
        for (int i = 0; i < block; ++i) {
            int id = start + i;
            if (id > shm->total_registros) break;

            string reg = generarRegistroAleatorio(id, idHijo);

            // Publicar en la SHM
            // PASO 1: Esperar a que el slot compartido esté vacío
            sem_wait_idx(semid, SEM_EMPTY_SLOT);

            // PASO 2: Proteger la escritura en el slot compartido con el mutex
            sem_wait_idx(semid, SEM_MUTEX);
            strncpy(shm->registro, reg.c_str(), sizeof(shm->registro) - 1);
            shm->registro[sizeof(shm->registro) - 1] = '\0';
            shm->id_publicado = id;
            // shm->nuevoRegistro ya no se usa
            sem_signal_idx(semid, SEM_MUTEX);

            // PASO 3: Avisar al coordinador que hay un nuevo registro disponible (el slot está lleno)
            sem_signal_idx(semid, SEM_FULL_SLOT);

            // La lógica de espera de "consumido" ya no es necesaria aquí;
            // SEM_EMPTY_SLOT se encarga de que no se sobrescriba antes de consumir.
        }

        usleep(50000); // pequeña pausa entre bloques
    }

    // Al terminar, reducir el contador de generadores activos
    sem_wait_idx(semid, SEM_MUTEX);
    shm->generadoresActivos--;
    sem_signal_idx(semid, SEM_MUTEX);

    _exit(0);
}

// ------------------------------- Coordinador ---------------------------------
static int runCoordinador(int N, int total, const string& rutaCSV) {
    // Crear el archivo CSV y escribir encabezado
    ofstream csv(rutaCSV, ios::out | ios::trunc);
    if (!csv) {
        cerr << "ERROR: No se pudo abrir el archivo de salida: " << rutaCSV << "\n";
        return 1;
    }
    csv << "ID,Nombre,Edad,Ciudad,Fuente\n";
    csv.flush();

    // Crear memoria compartida
    shmid = shmget(SHM_KEY, sizeof(SharedData), IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget");
        return 1;
    }

    shm = (SharedData*)shmat(shmid, nullptr, 0);
    if (shm == (void*)-1) {
        perror("shmat");
        return 1;
    }

    // Inicializar estructura compartida
    memset(shm, 0, sizeof(SharedData));
    shm->next_id = 1;
    shm->total_registros = total;
    shm->total_escritos = 0;
    shm->terminar = false;
    // shm->nuevoRegistro = false; // Ya no se usa
    shm->id_publicado = 0;
    shm->generadoresActivos = N;

    // Crear semáforos (SEM_MUTEX, SEM_FULL_SLOT, SEM_EMPTY_SLOT)
    semid = semget(SEM_KEY, SEM_COUNT, IPC_CREAT | 0666); // Usar SEM_COUNT
    if (semid == -1) {
        perror("semget");
        limpiarRecursos();
        return 1;
    }

    // Inicializar semáforos:
    // SEM_MUTEX=1 (libre)
    // SEM_FULL_SLOT=0 (el slot está vacío inicialmente)
    // SEM_EMPTY_SLOT=1 (el slot está disponible para un productor)
    {
        semun arg;
        unsigned short init[SEM_COUNT] = {1, 0, 1}; // Correcta inicialización para 3 semáforos
        arg.array = init;
        if (semctl(semid, 0, SETALL, arg) == -1) {
            perror("semctl SETALL");
            limpiarRecursos();
            return 1;
        }
    }

    // Manejo de Ctrl+C
    signal(SIGINT, sigint_handler);

    // Lanzar procesos generadores
    vector<pid_t> hijos;
    hijos.reserve(N);

    for (int i = 0; i < N; ++i) {
        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            shm->terminar = true;
            // Intentar terminar a los hijos ya lanzados antes de salir
            for (pid_t p : hijos) kill(p, SIGTERM);
            break;
        } else if (pid == 0) {
            procesoGenerador(i + 1);
        } else {
            hijos.push_back(pid);
        }
    }

    // 🧠 Bucle principal: consumir los registros publicados
    // Continuar mientras no se hayan escrito todos los registros o queden generadores activos
    while (true) {
        // Intentar esperar un registro disponible (slot lleno)
        // Usamos IPC_NOWAIT para poder revisar la condición de terminación
        sembuf op_full_wait = {SEM_FULL_SLOT, -1, IPC_NOWAIT};
        if (semop(semid, &op_full_wait, 1) == -1) {
            if (errno == EAGAIN) { // No hay elementos disponibles ahora mismo
                // Revisa condiciones de terminación bajo el mutex
                sem_wait_idx(semid, SEM_MUTEX);
                bool all_ids_assigned = (shm->next_id > shm->total_registros);
                bool all_generators_done = (shm->generadoresActivos == 0);
                bool all_records_written = (shm->total_escritos >= shm->total_registros);
                sem_signal_idx(semid, SEM_MUTEX);

                if (all_ids_assigned && all_generators_done && all_records_written) {
                    // Todos los IDs han sido asignados, todos los generadores han terminado,
                    // y todos los registros han sido escritos y consumidos.
                    break; // Salir del bucle
                }
                usleep(10000); // Pequeña pausa antes de reintentar si no hay nada disponible
                continue; // Continuar el bucle para re-chequear las condiciones
            }
            perror("semop SEM_FULL_SLOT wait");
            // Si ocurre un error real en semop, marcar terminar y salir
            sem_wait_idx(semid, SEM_MUTEX);
            shm->terminar = true;
            sem_signal_idx(semid, SEM_MUTEX);
            break;
        }

        // Si se llegó aquí, se adquirió SEM_FULL_SLOT, hay un elemento para consumir.
        // PASO 1: Proteger la lectura del slot compartido con el mutex
        sem_wait_idx(semid, SEM_MUTEX);
        string s = shm->registro;
        // shm->nuevoRegistro ya no se usa
        shm->total_escritos++;
        sem_signal_idx(semid, SEM_MUTEX); // Liberar el mutex global

        csv << s << "\n";
        csv.flush();

        // PASO 2: Señalar que el slot está ahora vacío, permitiendo a un productor llenarlo.
        sem_signal_idx(semid, SEM_EMPTY_SLOT);
    }

    // Marcar terminación global para los hijos que aún puedan estar activos
    // 🔸 Pausa para monitoreo manual de recursos
    cout << "\n⏸ Programa en pausa para monitoreo.\n";
    cout << "   Podés abrir otra terminal y ejecutar:\n";
    cout << "   - ipcs -m   (ver memoria compartida)\n";
    cout << "   - ipcs -s   (ver semáforos)\n";
    cout << "   - ps -eLf | grep " << getpid() << "   (ver procesos)\n";
    cout << "   Cuando termines de observar, presioná ENTER para continuar...\n";
    cin.get();  // Espera ENTER del usuario

    sem_wait_idx(semid, SEM_MUTEX);
    shm->terminar = true;
    sem_signal_idx(semid, SEM_MUTEX);

    // Despertar a TODOS los generadores que puedan estar bloqueados en SEM_EMPTY_SLOT
    // para que puedan ver la bandera `terminar` y salir limpiamente.
    for (int i = 0; i < N; ++i) {
        sem_signal_idx(semid, SEM_EMPTY_SLOT);
    }

    // Esperar a todos los hijos
    for (pid_t pid : hijos) {
        int status = 0;
        waitpid(pid, &status, 0);
    }

    csv.flush();
    csv.close();

    // 🔹 Pequeña pausa para asegurar cierre completo antes de la limpieza
    usleep(200000); // 0.2 seg

    // Limpieza final de recursos IPC
    limpiarRecursos(false);

    // 🟢 Resumen final
    cout << "\n✅ Archivo generado con éxito: " << rutaCSV
         << "\n📄 Registros totales: " << total // Usar 'total' que es el valor esperado
         << "\n👥 Generadores usados: " << N
         << "\n----------------------------------------\n";

    return 0;
}

// ---------------------------------- main -------------------------------------
static void print_help(const char* prog) {
    cerr << "Uso: " << prog << " <N_generadores> <total_registros> <salida.csv>\n"
         << "Ej.: " << prog << " 4 200 datos.csv\n";
}

int main(int argc, char* argv[]) {
    ios::sync_with_stdio(false);

    if (argc != 4) {
        print_help(argv[0]);
        return 1;
    }

    int N = atoi(argv[1]);
    int total = atoi(argv[2]);
    string rutaCSV = argv[3];

    if (N <= 0 || total <= 0) {
        cerr << "ERROR: N_generadores y total_registros deben ser enteros positivos.\n";
        print_help(argv[0]);
        return 1;
    }

    // Validación simple de nombre de archivo
    if (rutaCSV.find('/') == string::npos && rutaCSV.find('.') == string::npos) {
        cerr << "ADVERTENCIA: el nombre de archivo parece no tener extensión. Se recomienda .csv\n";
    }

    int rc = runCoordinador(N, total, rutaCSV);
    if (rc == 0) {
        cout << "OK: Generados " << total << " registros en '" << rutaCSV << "'.\n";
        cout << "Sugerencia de monitoreo: ipcs -m/-s, ps -eLf, htop, vmstat.\n";
    }
    return rc;
}

/**
    

 */