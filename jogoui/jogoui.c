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
#define BUFFER_SIZE 10
#define NUSERS 10


typedef struct _BufferCell {
	TCHAR  letra; // valor que o produtor gerou
} BufferCell;

typedef struct _SharedMem {
	unsigned int c;   // contador partilhado com o numero de consumidores   
	unsigned int wP;  // posicao do buffer circular para a escrita     
	unsigned int rP;  // posicao do buffer circular para a escrita  
	BufferCell buffer[BUFFER_SIZE]; // buffer circular
	char users[NUSERS];
	int nusers;
} SharedMem;

typedef struct _ControlData {
	unsigned int shutdown;  // flag "continua". 0 = continua, 1 = deve terminar 
	HANDLE hMapFile;        // ficheiro de memoria 
	SharedMem* sharedMem;   // memoria partilhada
	HANDLE hMutex;          // mutex 
	HANDLE hEvent;          // evento para leitura sincronizada
	HANDLE hPipe[NUSERS];   // array de handles para os pipes de cada jogador
	unsigned int nPipes; // maximo de pipes
	TCHAR username[26];         // Novo campo para guardar username do cliente
	TCHAR lastCommand[256];     // Último comando lido
	HANDLE hSendEvent; // evento para sinalizar que o produtor pode enviar dados
} ControlData;

typedef struct _PipeMsg {
	HANDLE hPipe; //necessario?
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

	return TRUE;
}


DWORD WINAPI consume(LPVOID p)
{
	ControlData* cdata = (ControlData*)p;
	BufferCell cell;
	int ranTime;

	while (1) {

		if (cdata->shutdown == 1) {
			_tprintf(TEXT("vou fechar!\n"));
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


DWORD WINAPI sendThread(LPVOID param) {
	ControlData* cdata = (ControlData*)param;
	PipeMsg msg;
	DWORD bytesRead = 0, cbWritten;
	OVERLAPPED OverlWr = { 0 };
	HANDLE hPipe = cdata->hPipe[0];

	// Primeiro envia o nome de usuário para login
	WaitForSingleObject(cdata->hMutex, INFINITE);
	ZeroMemory(&msg, sizeof(PipeMsg));
	_tcscpy_s(msg.username, 26, cdata->username);
	_tcscpy_s(msg.buff, 256, _T("login"));
	ReleaseMutex(cdata->hMutex);

	// Envia mensagem de login

	_tprintf(_T("Enviando login...\n"));

	// Envia mensagem de login
	if (!WriteFile(hPipe, &msg, sizeof(PipeMsg), &cbWritten, NULL)) {
		_tprintf(_T("[ERRO] ao enviar login! Error: %d\n"), GetLastError());
		return -1;
	}

	_tprintf(_T("Login enviado, aguardando resposta...\n"));


	// Espera resposta do login
	PipeMsg loginResponse;
	if (ReadFile(hPipe, &loginResponse, sizeof(PipeMsg), &bytesRead, NULL) && bytesRead > 0) {
		if (loginResponse.isUsernameInvalid) {
			_tprintf(_T("[ERRO] Nome de usuário inválido ou já em uso\n"));
			return -1;
		}
		_tprintf(_T("Login bem-sucedido como %s\n"), cdata->username);
	}

	while (1) {
		WaitForSingleObject(cdata->hSendEvent, INFINITE);

		WaitForSingleObject(cdata->hMutex, INFINITE);
		if (cdata->shutdown) { // Verifica se deve terminar
			ReleaseMutex(cdata->hMutex);
			break;
		}
		ZeroMemory(&msg, sizeof(PipeMsg));
		_tcscpy_s(msg.username, 26, cdata->username);
		_tcscpy_s(msg.buff, 256, cdata->lastCommand);
		ReleaseMutex(cdata->hMutex);

		// Envia comando
		_tprintf(_T("enviar comando! %s\n"), msg.buff);

		if (!WriteFile(hPipe, &msg, sizeof(PipeMsg), &cbWritten, NULL)) {
			_tprintf(_T("[ERRO] ao enviar comando! Error: %d\n"), GetLastError());
			break;
		}

		// Se for comando de saída, termina
		if (_tcscmp(msg.buff, _T("exit")) == 0)
			break;

		// Espera resposta (com timeout)
		PipeMsg response;
		DWORD waitResult = WaitForSingleObject(hPipe, 5000); // Timeout de 5 segundos
		if (waitResult == WAIT_OBJECT_0) {
			if (ReadFile(hPipe, &response, sizeof(PipeMsg), &bytesRead, NULL) && bytesRead > 0) {
				_tprintf(_T("Resposta: %s\n"), response.buff);
			}
		}
		else if (waitResult == WAIT_TIMEOUT) {
			_tprintf(_T("Timeout esperando por resposta\n"));
		}
		else {
			_tprintf(_T("Erro ao esperar por resposta\n"));
			break;
		}
	}

	return 0;
}

int _tmain(int argc, TCHAR* argv[]) {
	ControlData cdata;
	HANDLE hThread, hPipe, hEvent;
	TCHAR command[100], buf[256];
	DWORD dwMode;
	BOOL fSuccess = FALSE;
	PipeMsg msg;
	LPTSTR PIPE_NAME = _T("\\\\.\\pipe\\xpto");

#ifdef UNICODE
	_setmode(_fileno(stdin), _O_WTEXT);    // *** stdin  ***  
	_setmode(_fileno(stdout), _O_WTEXT);   // *** stdout ***
	_setmode(_fileno(stderr), _O_WTEXT);   // *** stderr ***
#endif

	if (argc != 2) {
		_tprintf(_T("Necessita inserir um nome"));
		exit(1);
	};




	cdata.shutdown = 0;
	/*/
		if (argc < 2) {
			_tprintf(TEXT("ERRO ARGS INVALIDOS\n"));
			exit(1);
		}
		username = argv[1];
		_tprintf(TEXT("Consumidor %s a iniciar...\n"), username);
		*/
		//inicializar

	if (!WaitNamedPipe(PIPE_NAME, 5000)) {
		_tprintf(_T("[ERRO] Ligação com o pipe! - Error: %d\n"), GetLastError());
		exit(-1);
	}
	hPipe = CreateFile(PIPE_NAME,
		GENERIC_READ |
		GENERIC_WRITE,
		0 | FILE_SHARE_READ | FILE_SHARE_WRITE,
		NULL, // default security attributes
		OPEN_EXISTING, // opens existing pipe
		0 | FILE_FLAG_OVERLAPPED, // default attributes
		NULL); // no template file

	if (hPipe == INVALID_HANDLE_VALUE) {
		_tprintf(_T("[ERRO] Criar pipe! Error: %d\n"), GetLastError());
		exit(-1);
	}

	dwMode = PIPE_READMODE_MESSAGE;
	fSuccess = SetNamedPipeHandleState(
		hPipe,    // handle para o pipe 
		&dwMode,  // Novo modo do pipe 
		NULL,     // N o   para mudar max. bytes 
		NULL);    // N o   para mudar max. timeout

	if (!fSuccess) {
		CloseHandle(hPipe);
		return -1;
	}

	cdata.hPipe[0] = hPipe;
	cdata.nPipes = 1; // numero de pipes
	// login attempt

	TCHAR loginMsg[256];
	DWORD bytesWritten;
	DWORD bytesRead;


	_tprintf(_T("A fazer login como %s ...\n"), argv[1]);

	ZeroMemory(&msg, sizeof(PipeMsg));
	_tcsncpy_s(cdata.username, 26, argv[1],_TRUNCATE);

	cdata.hSendEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	if (cdata.hSendEvent == NULL) {
		_tprintf(_T("[ERRO] criar evento de envio! Error: %d\n"), GetLastError());
		return -1;
	}

	HANDLE hSendThread = CreateThread(NULL, 0, sendThread, &cdata, 0, NULL);
	if (hSendThread == NULL) {
		_tprintf(_T("Erro ao criar thread de envio\n"));
		CloseHandle(hPipe);
		return -1;
	}

	// Espera um pouco para ver se o login foi bem-sucedido
	if (WaitForSingleObject(hSendThread, 3000) == WAIT_OBJECT_0) {
		DWORD exitCode;
		GetExitCodeThread(hSendThread, &exitCode);
		if (exitCode != 0) {
			_tprintf(_T("Erro no login, terminando...\n"));
			CloseHandle(hPipe);
			return -1;
		}
	}

	if (!initMemAndSync(&cdata)) {
		_tprintf(TEXT("Error creating/opening shared memory and synchronization mechanisms.\n"));
		exit(-1);
	}

	hEvent = OpenEvent(EVENT_ALL_ACCESS, FALSE, EVENT_NAME);
	if (hEvent == NULL) {
		_tprintf_s(_T("Erro ao abrir evento: %ld\n"), GetLastError());
		exit(-1);
	}
	cdata.hEvent = hEvent;

	WaitForSingleObject(cdata.hMutex, INFINITE);
	ReleaseMutex(cdata.hMutex);

	hThread = CreateThread(NULL, 0, consume, &cdata, 0, NULL);

	_tprintf(TEXT("Type in 'exit' to leave.\n"));

	do {
		_getts_s(command, 100);
		WaitForSingleObject(cdata.hMutex, INFINITE);
		_tcscpy_s(cdata.lastCommand, 256, command);
		ReleaseMutex(cdata.hMutex);
		SetEvent(cdata.hSendEvent);

		if (_tcscmp(command, TEXT("exit")) == 0) {
			WaitForSingleObject(hSendThread, 2000); // Espera a thread de envio terminar
			break;
		}
	} while (1);

	cdata.shutdown = 1; //flag para terminar a thread
	WaitForSingleObject(hThread, INFINITE); //espera que a thread termine


	//fechar os handles para terminar
	CloseHandle(hThread);
	CloseHandle(hPipe);
	UnmapViewOfFile(cdata.sharedMem);
	CloseHandle(cdata.hMapFile);
	CloseHandle(cdata.hMutex);
	return 0;
}
