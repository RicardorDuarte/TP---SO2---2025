#include <windows.h>
#include <stdio.h>
#include <conio.h>
#include <tchar.h>
#include <process.h>
#include <fcntl.h> 
#include <io.h>

#define SHM_NAME TEXT("SHM_PC")           // nome da memoria partilhada
#define MUTEX_NAME TEXT("MUTEX")          // nome do mutex    
#define SEM_WRITE_NAME TEXT("SEM_WRITE")  // nome do semaforo de escrita
#define SEM_READ_NAME TEXT("SEM_READ")    // nome do semaforo de leitura
#define EVENT_NAME TEXT("EVENT")          // nome do evento
#define BUFFER_SIZE 6
#define NUSERS 10

typedef struct _BufferCell {
    TCHAR  letra; // valor que o produtor gerou
} BufferCell;

typedef struct _SharedMem {
    unsigned int c;   //    
    unsigned int wP;  // posicao do buffer escrita     
    unsigned int rP;  // posicao do buffer leitura
    BufferCell buffer[BUFFER_SIZE]; // buffer circular
    char users[NUSERS][25]; // array de strings para os nomes dos jogadores
    int pontuacao[NUSERS]; // array de inteiros para as pontuações dos jogadores
    int nusers;
} SharedMem;

typedef struct _ControlData {
    unsigned int shutdown;  // flag "continua". 0 = continua, 1 = deve terminar 
    HANDLE hMapFile;        // ficheiro de memoria 
    SharedMem* sharedMem;   // memoria partilhada
    HANDLE hMutex;          // mutex 
    HANDLE hEvent;          // evento para leitura sincronizada
    HANDLE hPipe[NUSERS];   // array de handles para os pipes de cada jogador
    unsigned int nPipes;    // maximo de pipes
    TCHAR username[26];     // username do bot
    TCHAR lastCommand[256]; // Último comando lido
    HANDLE hSendEvent;      // evento para sinalizar que o produtor pode enviar dados
} ControlData;

typedef struct _BotData {
	ControlData* cdata;     // ponteiro para os dados de controlo
	unsigned int velocidade; // palavras que o bot pode enviar
} BotData;

typedef struct _PipeMsg {
    HANDLE hPipe;           // handle do pipe
    TCHAR buff[256];        // mensagem/palavra
    BOOL isUsernameInvalid; // flag para username inválido
    TCHAR username[26];     // username
} PipeMsg;

BOOL initMemAndSync(ControlData* cdata) {
    cdata->hMapFile = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, SHM_NAME);
    if (cdata->hMapFile == NULL) {
        cdata->hMapFile = CreateFileMapping(
            INVALID_HANDLE_VALUE,
            NULL,
            PAGE_READWRITE,
            0,
            sizeof(SharedMem),
            SHM_NAME);
    }

    if (cdata->hMapFile == NULL) {
        _tprintf(TEXT("Error: CreateFileMapping (%d)\n"), GetLastError());
        return FALSE;
    }

    cdata->sharedMem = (SharedMem*)MapViewOfFile(
        cdata->hMapFile,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        sizeof(SharedMem));

    if (cdata->sharedMem == NULL) {
        _tprintf(TEXT("Erro MapViewOf file %d"), GetLastError());
        CloseHandle(cdata->hMapFile);
        return FALSE;
    }

    cdata->hMutex = CreateMutex(NULL, FALSE, MUTEX_NAME);
    if (cdata->hMutex == NULL) {
        _tprintf(TEXT("ERRO: %d"), GetLastError());
        UnmapViewOfFile(cdata->sharedMem);
        CloseHandle(cdata->hMapFile);
        return FALSE;
    }

    return TRUE;
}

DWORD WINAPI lerMemPart(LPVOID p) {
    ControlData* cdata = (ControlData*)p;

    while (cdata->shutdown == 0) {
        if (cdata->shutdown == 1) {
            _tprintf(TEXT("Fechando leitura da memória partilhada!\n"));
            return 0;
        }

        DWORD waitLetter = WaitForSingleObject(cdata->hEvent, 15000);
        if (waitLetter == WAIT_TIMEOUT) {
            _tprintf(TEXT("Timeout a espera por evento\n"));
            continue;
        }

        _tprintf(TEXT("\nARRAY:\n"));
        for (int i = 0; i < BUFFER_SIZE; i++) {
            WaitForSingleObject(cdata->hMutex, INFINITE);
            _tprintf(TEXT("%c\t"), cdata->sharedMem->buffer[i].letra);
            ReleaseMutex(cdata->hMutex);
        }
        _tprintf(TEXT("\n"));

        if (cdata->sharedMem->rP == BUFFER_SIZE) {
            WaitForSingleObject(cdata->hMutex, INFINITE);
            cdata->sharedMem->rP = 0;
            ReleaseMutex(cdata->hMutex);
        }
    }
    return 0;
}

DWORD WINAPI recebePipe(LPVOID param) {
    ControlData* cdata = (ControlData*)param;
    PipeMsg response;
    DWORD bytesRead;

    while (cdata->shutdown == 0) {
        if (ReadFile(cdata->hPipe[0], &response, sizeof(PipeMsg), &bytesRead, NULL)) {
            if (bytesRead > 0) {
                _tprintf(_T("Resposta recebida: %s\n"), response.buff);

                if (_tcscmp(response.buff, _T("close")) == 0) {
                    _tprintf(_T("Expulso pelo administrador\n"));
                    cdata->shutdown = 1;
                    return 0;
                }
            }
        }
        else {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE) {
                _tprintf(_T("Servidor desconectou\n"));
                cdata->shutdown = 1;
                return 0;
            }
            else {
                _tprintf(_T("Erro ao ler resposta (%d)\n"), err);
            }
            return 1;
        }
    }
    return 0;
}

DWORD WINAPI enviaPalavras(LPVOID param) {
    BotData* bdata = (BotData*)param;
    PipeMsg msg;
    DWORD bytesWritten;

    // Lista de palavras que o bot vai enviar
    const TCHAR* palavras[] = {
        _T("carro"),
        _T("mae"),
        _T("pai")
    };
    int numPalavras = sizeof(palavras) / sizeof(palavras[0]);

    // Intervalo entre envios (em milissegundos)


    _tcscpy_s(msg.username, 26, bdata->cdata->username);

    while (bdata->cdata->shutdown == 0) {
        // Seleciona uma palavra aleatória da lista
        int idx = rand() % numPalavras;
        _tcscpy_s(msg.buff, 256, palavras[idx]);

        _tprintf(_T("Enviando palavra: %s\n"), msg.buff);
        if (!WriteFile(bdata->cdata->hPipe[0], &msg, sizeof(PipeMsg), &bytesWritten, NULL)) {
            _tprintf(_T("[ERRO] ao enviar palavra! Error: %d\n"), GetLastError());
            break;
        }
        Sleep(1000*bdata->velocidade);

    }
    return 0;
}

int _tmain(int argc, TCHAR* argv[]) {
    ControlData cdata;
	BotData botData;
    HANDLE hThread, hPipe, hEvent;
    LPTSTR PIPE_NAME = _T("\\\\.\\pipe\\xpto");

#ifdef UNICODE
    _setmode(_fileno(stdin), _O_WTEXT);
    _setmode(_fileno(stdout), _O_WTEXT);
    _setmode(_fileno(stderr), _O_WTEXT);
#endif

    if (argc != 3) {
        _tprintf(_T("Necessita inserir um nome e velocidade\n"));
        exit(1);
    }
    // Inicialização
    botData.cdata = &cdata;
    cdata.shutdown = 0;
    _tcsncpy_s(cdata.username, 26, argv[1], _TRUNCATE);
    botData.velocidade= _tstoi(argv[2]);
    srand(GetTickCount()); // Para seleção aleatória de palavras

    // Conexão com o pipe
    if (!WaitNamedPipe(PIPE_NAME, 5000)) {
        _tprintf(_T("[ERRO] Pipe não disponível - Error: %d\n"), GetLastError());
        exit(-1);
    }

    hPipe = CreateFile(
        PIPE_NAME,
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        NULL);

    if (hPipe == INVALID_HANDLE_VALUE) {
        _tprintf(_T("[ERRO] Criar pipe! Error: %d\n"), GetLastError());
        exit(-1);
    }

    DWORD dwMode = PIPE_READMODE_MESSAGE;
    if (!SetNamedPipeHandleState(hPipe, &dwMode, NULL, NULL)) {
        CloseHandle(hPipe);
        _tprintf(_T("[ERRO] Configurar pipe! Error: %d\n"), GetLastError());
        return -1;
    }

    cdata.hPipe[0] = hPipe;
    cdata.nPipes = 1;

    // Processo de login
    PipeMsg loginMsg, loginResponse;
    DWORD bytesWritten, bytesRead;

    ZeroMemory(&loginMsg, sizeof(PipeMsg));
    _tcscpy_s(loginMsg.username, 26, cdata.username);
    _tcscpy_s(loginMsg.buff, 256, _T("login"));

    _tprintf(_T("Enviando login...\n"));
    if (!WriteFile(hPipe, &loginMsg, sizeof(PipeMsg), &bytesWritten, NULL)) {
        _tprintf(_T("[ERRO] ao enviar login! Error: %d\n"), GetLastError());
        CloseHandle(hPipe);
        return -1;
    }

    _tprintf(_T("Aguardando resposta de login...\n"));
    if (!ReadFile(hPipe, &loginResponse, sizeof(PipeMsg), &bytesRead, NULL)) {
        _tprintf(_T("[ERRO] ao ler resposta de login! Error: %d\n"), GetLastError());
        CloseHandle(hPipe);
        return -1;
    }

    if (loginResponse.isUsernameInvalid) {
        _tprintf(_T("[ERRO] Nome de usuário inválido ou já em uso\n"));
        CloseHandle(hPipe);
        return -1;
    }

    _tprintf(_T("Login bem-sucedido como %s\n"), cdata.username);

    // Inicia thread para receber respostas
    HANDLE hReceiveThread = CreateThread(NULL, 0, recebePipe, &cdata, 0, NULL);
    if (hReceiveThread == NULL) {
        _tprintf(_T("Erro ao criar thread de recepção\n"));
        CloseHandle(hPipe);
        return -1;
    }

    // Inicializa memória compartilhada
    if (!initMemAndSync(&cdata)) {
        _tprintf(TEXT("Erro ao inicializar memória compartilhada\n"));
        CloseHandle(hPipe);
        CloseHandle(hReceiveThread);
        exit(-1);
    }

    // Thread para enviar palavras automaticamente
    HANDLE hEnviaThread = CreateThread(NULL, 0, enviaPalavras, &botData, 0, NULL);
    if (hEnviaThread == NULL) {
        _tprintf(_T("Erro ao criar thread de envio\n"));
        CloseHandle(hPipe);
        CloseHandle(hReceiveThread);
        exit(-1);
    }

    hEvent = OpenEvent(EVENT_ALL_ACCESS, FALSE, EVENT_NAME);
    if (hEvent == NULL) {
        _tprintf_s(_T("Erro ao abrir evento: %ld\n"), GetLastError());
        exit(-1);
    }
    cdata.hEvent = hEvent;

    hThread = CreateThread(NULL, 0, lerMemPart, &cdata, 0, NULL);
    if (hThread == NULL) {
        _tprintf(TEXT("Erro ao criar thread de leitura da memória partilhada (%d)\n"), GetLastError());
        CloseHandle(hPipe);
        CloseHandle(hReceiveThread);
        CloseHandle(hEnviaThread);
        exit(-1);
    }

    // Aguarda as threads terminarem
    WaitForSingleObject(hEnviaThread, INFINITE);
    WaitForSingleObject(hReceiveThread, INFINITE);

    // Limpeza
    cdata.shutdown = 1;
    CloseHandle(cdata.hEvent);
    CloseHandle(hThread);
    CloseHandle(hEnviaThread);
    _tprintf(_T("Encerrando bot...\n"));
    CloseHandle(hReceiveThread);
    CloseHandle(hPipe);
    UnmapViewOfFile(cdata.sharedMem);
    CloseHandle(cdata.hMapFile);
    CloseHandle(cdata.hMutex);

    return 0;
}
