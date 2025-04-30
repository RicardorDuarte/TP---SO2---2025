#include <windows.h>
#include <stdio.h>
#include <conio.h>
#include <tchar.h>
#include <process.h>
#include <fcntl.h> 
#include <io.h>

#define SHM_NAME TEXT("SHM_PC")           // nome da memoria partilhada
#define MUTEX_NAME TEXT("MUTEX")          // nome do mutex   -> em casa devem pensar numa solução para ter mutex´s distintos de forma a não existir perda de performance 
#define SEM_WRITE_NAME TEXT("SEM_WRITE")  // nome do semaforo de escrita
#define SEM_READ_NAME TEXT("SEM_READ")    // nome do semaforo de leitura
#define BUFFER_SIZE 10


typedef struct _BufferCell {
	wchar_t  letra; // valor que o produtor gerou
} BufferCell;

typedef struct _SharedMem {
	unsigned int c;   // contador partilhado com o numero de consumidores   
	unsigned int wP;  // posicao do buffer circular para a escrita     
	unsigned int rP;  // posicao do buffer circular para a escrita  
	BufferCell buffer[BUFFER_SIZE]; // buffer circular
} SharedMem;

typedef struct _ControlData {
	unsigned int shutdown;  // flag "continua". 0 = continua, 1 = deve terminar
	unsigned int id;        // id do processo  
	unsigned int count;     // contador do numero de vezes  
	unsigned int sum;       // somatorio de todos os proc consumidores 
	HANDLE hMapFile;        // ficheiro de memoria 
	SharedMem* sharedMem;   // memoria partilhada
	HANDLE hMutex;          // mutex - trabalho de casa -> acrescentar os outros 2 mutexes
	HANDLE hWriteSem;       // sem�foro "aguarda por items escritos"
	HANDLE hReadSem;        // sem�foro "aguarda por posições vazias"
} ControlData;

BOOL initMemAndSync(ControlData* cdata)
{
	//Creates or opens a named or unnamed file mapping object for a specified file.
	//Criar ou abrir um ficheiro para mapear em memoria
	BOOL firstProcess = FALSE;
	cdata->hMapFile = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, SHM_NAME);//SE CORRER BEM É PORQUE JÁ EXISTE, SE FOR NULL CRIA EM BAIXO
	if (cdata->hMapFile == NULL) {//se for null cria o
		cdata->hMapFile = CreateFileMapping(
			INVALID_HANDLE_VALUE, //como aqui nao ha ficheiro para interligar, nao colocamos nada
			NULL, // ATRIBUTO DE SEGURANCA DEFAULT
			PAGE_READWRITE,//PERMISSOES DE VISTAS
			0,//tamanho inicial
			sizeof(SharedMem), //sizeof(SharedMsg) AQUI É O TAMANHO
			SHM_NAME); //este nome é muito importante para saber qual o hMapFile a usar/chamar noutros processos
		firstProcess = TRUE;//se foi criada é o primeiro processo
	}
	if (cdata->hMapFile == NULL)
	{
		_tprintf(TEXT("Error: CreateFileMapping (%d)\n"), GetLastError());
		return FALSE;
	}

	//Maps a view of a file mapping into the address space of a calling process
	//isto serve para colocar a mensagem escrita em memoria partilhada, para podermos aceder noutros processos
	cdata->sharedMem = (SharedMem*)MapViewOfFile(cdata->hMapFile,//aquilo que pretendemos mapear
		FILE_MAP_ALL_ACCESS, //permissoes de acesso, tipo de acesso
		0,//USAR 0 SE O FICHEIRO FOR MENOR QUE DGB
		0,//de onde começamos a mapear
		sizeof(SharedMem)); //tamanho max

	if (cdata->sharedMem == NULL) {
		_tprintf(TEXT("Erro MapViewOf file %d"), GetLastError());
		CloseHandle(cdata->hMapFile);
		return FALSE;
	}

	//inicializa
	if (firstProcess) {

	}

	//criar o mutex, uma vez que varios processos podem estar aceder ao mesmo espaço de memoria 
	cdata->hMutex = CreateMutex(NULL,//atributos de segurança
		FALSE,
		MUTEX_NAME); // nome do mutex para permitir a partilha entre processos diferentes

	if (cdata->hMutex == NULL) {
		_tprintf(TEXT("ERRO: %d"), GetLastError());
		UnmapViewOfFile(cdata->sharedMem);
		CloseHandle(cdata->hMapFile);
		return FALSE;
	}

	//semaforo para escrita
	cdata->hWriteSem = CreateSemaphore(NULL, BUFFER_SIZE, BUFFER_SIZE, SEM_WRITE_NAME);//cria o semaforo e deixa escrever ate ao maximo logo à cabeca

	if (cdata->hWriteSem != NULL) {//eu acho que faz sentido ser assim, so cria o de leitura se o de escrita tiver sido criado
		cdata->hReadSem = CreateSemaphore(NULL, 0, BUFFER_SIZE, SEM_READ_NAME);//aquele zero é oq ele deixa passar no inicio, que é nada porque não há nada escrito
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

//thread que vai estar a consultar o buffer circular
DWORD WINAPI consume(LPVOID p)
{//ACHO QUE TA FEITO
	ControlData* cdata = (ControlData*)p;
	BufferCell cell;
	int ranTime;

	while (1) {

		if (cdata->shutdown == 1) {
			_tprintf(TEXT("Consumidor %d a terminar\n"), cdata->id);
			return 0; //flag para terminar
		}
		WaitForSingleObject(cdata->hReadSem, INFINITE); //estou a espera que possa ler os numeros mandados
		WaitForSingleObject(cdata->hMutex, INFINITE);//mexer na memoria, zona critica


		_tprintf(TEXT("\nARRAY:\n"));
		for (int i = 0; i < BUFFER_SIZE; i++) {
			_tprintf(TEXT("%c\t"), cdata->sharedMem->buffer[i].letra);
		}
		_tprintf(TEXT("\n"));

		//CopyMemory(&cell, &(cdata->sharedMem->buffer[(cdata->sharedMem->rP)++]), sizeof(BufferCell)); //recebo da memoria partilhada o nr
		//qnd quisermos retirar da memória partilhada usar codigo a cima


		if (cdata->sharedMem->rP == BUFFER_SIZE)
			cdata->sharedMem->rP = 0;//volta a ler do principio, caso chegue ao limite

		ReleaseMutex(cdata->hMutex);//fim zona critica
		ReleaseSemaphore(cdata->hWriteSem, 1, NULL);// liberto um produtor, porque ja leu oq estava escrito

		cdata->count++;//nr de itens
	}
	return 0;
}

int _tmain(int argc, TCHAR* argv[]) {
	//OQ FAZER AQUI DE DIFERENTE DO PRODUTOR
	//ARRANCAR COM A THREAD CONSOME, O CONTADOR É O C INVES DE P
	//DIZ QUE CONSUMIU X ITENS MAIS NADA
	//O CONSUMIDOR E O PRODUTOR TEM Q ESTAR EM PROJETOS DIFERENTES !!!!!, PARA COMPILAR
	ControlData cdata;
	HANDLE hThread;
	TCHAR command[100];
	TCHAR* username;
#ifdef UNICODE
	_setmode(_fileno(stdin), _O_WTEXT);    // *** stdin  ***  
	_setmode(_fileno(stdout), _O_WTEXT);   // *** stdout ***
	_setmode(_fileno(stderr), _O_WTEXT);   // *** stderr ***
#endif

	cdata.shutdown = 0;
	cdata.count = 0;
	cdata.sum = 0;
	/*/
		if (argc < 2) {
			_tprintf(TEXT("ERRO ARGS INVALIDOS\n"));
			exit(1);
		}
		username = argv[1];
		_tprintf(TEXT("Consumidor %s a iniciar...\n"), username);
		*/
		//inicializar
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
	UnmapViewOfFile(cdata.sharedMem);
	CloseHandle(cdata.hMapFile);
	CloseHandle(cdata.hMutex);
	CloseHandle(cdata.hWriteSem);
	CloseHandle(cdata.hReadSem);
	return 0;
}
