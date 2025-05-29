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
    TCHAR  letra; 
} BufferCell;

typedef struct _SharedMem {
    unsigned int c;      
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
    HANDLE hEvSai;          // evento para desbloquear o gets
    HANDLE hPipe[NUSERS];   // array de handles para os pipes de cada jogador
    unsigned int nPipes; // maximo de pipes
    TCHAR username[26];         // Novo campo para guardar username do cliente
    TCHAR lastCommand[256];     // Último comando lido
} ControlData;

typedef struct _PipeMsg {
    HANDLE hPipe; 
    TCHAR buff[256];
    BOOL isUsernameInvalid;
    TCHAR username[26];
} PipeMsg;


BOOL initMemAndSync(ControlData* cdata)
{
    cdata->hMapFile = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, SHM_NAME);//se correr bem ja existe, se não, cria depois
    if (cdata->hMapFile == NULL) {//se for null cria
        cdata->hMapFile = CreateFileMapping(
            INVALID_HANDLE_VALUE, //nao ha ficheiro para interligar, nao colocamos nada
            NULL, // atributo seguranca default
            PAGE_READWRITE,//permissoes
            0,//tamanho inicial
            sizeof(SharedMem), // tamanho
            SHM_NAME);
    }
    if (cdata->hMapFile == NULL)
    {
        _tprintf(TEXT("Error: CreateFileMapping (%d)\n"), GetLastError());
        return FALSE;
    }


    cdata->sharedMem = (SharedMem*)MapViewOfFile(cdata->hMapFile,//mapeia a memoria
        FILE_MAP_ALL_ACCESS, //permissoes / tipo de acesso
        0,//0 if < 4GB
        0,//de onde começamos a mapear
        sizeof(SharedMem)); //tamanho max

    if (cdata->sharedMem == NULL) {
        _tprintf(TEXT("Erro MapViewOf file %d"), GetLastError());
        CloseHandle(cdata->hMapFile);
        return FALSE;
    }

    cdata->hMutex = CreateMutex(NULL,//seg
        FALSE,
        MUTEX_NAME);

    if (cdata->hMutex == NULL) {
        _tprintf(TEXT("ERRO: %d"), GetLastError());
        UnmapViewOfFile(cdata->sharedMem);
        CloseHandle(cdata->hMapFile);
        return FALSE;
    }

    cdata->hEvSai = CreateEvent(NULL, TRUE, FALSE, NULL); // sinalizar para saída

    return TRUE;
}


DWORD WINAPI lerMemPart(LPVOID p)
{
    ControlData* cdata = (ControlData*)p;
    BufferCell cell;
    int ranTime;

    while (cdata->shutdown == 0) {

        if (cdata->shutdown == 1) {
            _tprintf(TEXT("vou fechar mem partilhada!\n"));
            return 0;
        }

        //espera que haja algo para ler
        DWORD waitLetter = WaitForSingleObject(cdata->hEvent, 15000);

        if (waitLetter == WAIT_TIMEOUT) {
            _tprintf(TEXT("Timeout a espera por evento\n"));
            continue;
        }

        _tprintf(TEXT("\nARRAY:\n"));
        for (int i = 0; i < BUFFER_SIZE; i++) {
            WaitForSingleObject(cdata->hMutex, INFINITE);//mexer na memoria
            _tprintf(TEXT("%c\t"), cdata->sharedMem->buffer[i].letra);
            ReleaseMutex(cdata->hMutex);//fim zona critica

        }
        _tprintf(TEXT("\n"));

        //CopyMemory(&cell, &(cdata->sharedMem->buffer[(cdata->sharedMem->rP)++]), sizeof(BufferCell)); //recebo da memoria partilhada o nr
        //qnd quisermos retirar da memória partilhada usar codigo a cima

        if (cdata->sharedMem->rP == BUFFER_SIZE) {
            WaitForSingleObject(cdata->hMutex, INFINITE);
            cdata->sharedMem->rP = 0;//volta a ler do principio, caso chegue ao limite
            ReleaseMutex(cdata->hMutex);//fim zona critica
        }

        WaitForSingleObject(cdata->hMutex, INFINITE);
        ReleaseMutex(cdata->hMutex);//fim zona critica
    }
    return 0;
}
DWORD WINAPI recebePipe(LPVOID param) {
    ControlData* cdata = (ControlData*)param;
    PipeMsg response;
    DWORD bytesRead;

    while (cdata->shutdown == 0) {
        if (cdata->shutdown) {
            return 0;
        }

        // Espera por respostas do servidor
        if (ReadFile(cdata->hPipe[0], &response, sizeof(PipeMsg), &bytesRead, NULL)) {
            if (bytesRead > 0) {
                _tprintf(_T("Resposta recebida: %s\n"), response.buff);

                // Se for uma mensagem de shutdown
                if (_tcscmp(response.buff, _T("close")) == 0) {
                    _tprintf(_T("Expulso pelo administrador\n"));
                    cdata->shutdown = 1;
                    break;
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

DWORD WINAPI comunica(LPVOID param) {
    ControlData* cdata = (ControlData*)param;
    TCHAR command[256]; //tem de ser 256 ou aprte-se tudo, ADORO UNICODE :DDDDDDDDDD
    PipeMsg msg;
    DWORD bytesWritten;

    _tprintf(TEXT("Para comandos digite :(comando)\nPara jogar introduza a palavra\n"));
    _tcscpy_s(msg.username, 26, cdata->username);  // Copia o username

    while (cdata->shutdown == 0) {
        _getts_s(command, 100);
        if (command[0] == _T(':')) {
            PipeMsg cmdMsg;
            ZeroMemory(&cmdMsg, sizeof(PipeMsg));
            _tcscpy_s(cmdMsg.username, 26, cdata->username);
            _tcscpy_s(cmdMsg.buff, 256, command);

            if (_tcscmp(command, _T(":sair")) != 0 && _tcscmp(command, _T(":jogs")) != 0 && _tcscmp(command, _T(":pont")) != 0) {
                _tprintf(_T("Comando inválido\n"));
                continue;
            }


            _tprintf(_T("Enviando comando: %s\nUsername %s\n"), cmdMsg.buff, cmdMsg.username);
            if (!WriteFile(cdata->hPipe[0], &cmdMsg, sizeof(PipeMsg), &bytesWritten, NULL)) {
                _tprintf(_T("[ERRO] ao enviar comando! Error: %d\n"), GetLastError());
                break;
            }
            if (_tcscmp(command, TEXT(":sair")) == 0) {
                break;
            }
        }
        else {
            // Envia a palavra jogada
            ZeroMemory(&msg, sizeof(PipeMsg));
            _tcscpy_s(msg.username, 26, cdata->username);
            _tcscpy_s(msg.buff, 256, command);
            _tprintf(_T("Enviando palavra: %s\n"), msg.buff);
            if (!WriteFile(cdata->hPipe[0], &msg, sizeof(PipeMsg), &bytesWritten, NULL)) {
                _tprintf(_T("[ERRO] ao enviar palavra! Error: %d\n"), GetLastError());
                break;
            }
        }
    }
    return 0;
}




int _tmain(int argc, TCHAR* argv[]) {
    ControlData cdata;
    HANDLE hThread, hPipe, hEvent;
    TCHAR command[100];
    LPTSTR PIPE_NAME = _T("\\\\.\\pipe\\pipeTP");

#ifdef UNICODE
    _setmode(_fileno(stdin), _O_WTEXT);
    _setmode(_fileno(stdout), _O_WTEXT);
    _setmode(_fileno(stderr), _O_WTEXT);
#endif

    if (argc != 2) {
        _tprintf(_T("Necessita inserir um nome\n"));
        exit(1);
    }

    cdata.shutdown = 0;
    _tcsncpy_s(cdata.username, 26, argv[1], _TRUNCATE);

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

    // Processo de login no main()
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
    HANDLE hComunicaThread = CreateThread(NULL, 0, comunica, &cdata, 0, NULL);
    if (hComunicaThread == NULL) {
        _tprintf(_T("Erro ao criar thread de comunicação\n"));
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
        exit(-1);
    }

    WaitForSingleObject(hComunicaThread, INFINITE);
    WaitForSingleObject(hReceiveThread, 2000);



    // Limpeza
    cdata.shutdown = 1;
    CloseHandle(cdata.hEvent);
    CloseHandle(hThread);
    CloseHandle(hComunicaThread);
    _tprintf(_T("Vou encerrar!!\n"));
    CloseHandle(hReceiveThread);
    CloseHandle(hPipe);
    UnmapViewOfFile(cdata.sharedMem);
    CloseHandle(cdata.hMapFile);
    CloseHandle(cdata.hMutex);

    return 0;
}
