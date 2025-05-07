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
#define BUFFER_SIZE 10



typedef struct _BufferCell {
	TCHAR  letra; // 
} BufferCell;

typedef struct _SharedMem {
	unsigned int c;   // contador partilhado com o numero de consumidores   
	unsigned int wP;  // posicao do buffer para escrita     
	unsigned int rP;  // posicao do buffer para escrita  
	BufferCell buffer[BUFFER_SIZE]; // buffer
} SharedMem;

typedef struct _ControlData {
	unsigned int shutdown;  //  0 = continua, 1 = terminar
	unsigned int id;        // id do processo  
	unsigned int count;     // contador do numero de users  
	HANDLE hMapFile;        // ficheiro de memoria 
	SharedMem* sharedMem;   // memoria partilhada
	HANDLE hMutex;          // mutex
	HANDLE hWriteSem;       // semaforo 
	HANDLE hReadSem;        // semaforo 
} ControlData;

BOOL initMemAndSync(ControlData* cdata)
{
	BOOL firstProcess = FALSE;
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

	//semaforo para escrita
	cdata->hWriteSem = CreateSemaphore(NULL, BUFFER_SIZE, BUFFER_SIZE, SEM_WRITE_NAME);
	if (cdata->hWriteSem != NULL) {
		cdata->hReadSem = CreateSemaphore(NULL, 0, BUFFER_SIZE, SEM_READ_NAME);
	}

	if (cdata->hReadSem == NULL) {
		_tprintf(TEXT("ERRO NO SEMAFORO LEITURA %d "), GetLastError());
		UnmapViewOfFile(cdata->sharedMem);
		CloseHandle(cdata->hMapFile);
		CloseHandle(cdata->hMutex);
		CloseHandle(cdata->hWriteSem);
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
		cdata->count++;//nr de itens
		ReleaseMutex(cdata->hMutex);//fim zona critica
		Sleep(3000);
	}
	return 0;
}

int _tmain(int argc, TCHAR* argv[]) {
	//OQ FAZER AQUI DE DIFERENTE DO PRODUTOR
	//ARRANCAR COM A THREAD CONSOME, O CONTADOR É O C INVES DE P
	//DIZ QUE CONSUMIU X ITENS MAIS NADA
	//O CONSUMIDOR E O PRODUTOR TEM Q ESTAR EM PROJETOS DIFERENTES !!!!!, PARA COMPILAR
	ControlData cdata;
	HANDLE hThread, hPipe;
	TCHAR command[100], buf[256];
	TCHAR* username;
	LPTSTR PIPE_NAME = _T("\\\\.\\pipe\\xpto");

#ifdef UNICODE
	_setmode(_fileno(stdin), _O_WTEXT);    // *** stdin  ***  
	_setmode(_fileno(stdout), _O_WTEXT);   // *** stdout ***
	_setmode(_fileno(stderr), _O_WTEXT);   // *** stderr ***
#endif

	cdata.shutdown = 0;
	cdata.count = 0;
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
	
	if (hPipe == NULL) {
		_tprintf(_T("[ERRO] Criar pipe! Error: %d\n"), GetLastError());
		exit(-1);
	}
	_tprintf(_T("Ligação a escritor bem sucedida!\n"));



	if (!initMemAndSync(&cdata)) {
		_tprintf(TEXT("Error creating/opening shared memory and synchronization mechanisms.\n"));
		exit(1);
	}

	WaitForSingleObject(cdata.hMutex, INFINITE);
	cdata.id = ++(cdata.sharedMem->c); // incrementa o contador partilhado com o numero de consumidores
	ReleaseMutex(cdata.hMutex);

	hThread = CreateThread(NULL, 0, consume, &cdata, 0, NULL);

	_tprintf(TEXT("Type in 'exit' to leave.\n"));

	do {
		_getts_s(command, 100);

	} while (_tcscmp(command, TEXT("exit")) != 0);

	ReleaseSemaphore(cdata.hReadSem, 1, NULL); //liberto o semaforo de leitura
	cdata.shutdown = 1; //flag para terminar a thread
	WaitForSingleObject(hThread, INFINITE); //espera que a thread termine


	//fechar os handles para terminar
	CloseHandle(hThread);
	CloseHandle(hPipe);
	UnmapViewOfFile(cdata.sharedMem);
	CloseHandle(cdata.hMapFile);
	CloseHandle(cdata.hMutex);
	CloseHandle(cdata.hWriteSem);
	CloseHandle(cdata.hReadSem);
	return 0;
}
