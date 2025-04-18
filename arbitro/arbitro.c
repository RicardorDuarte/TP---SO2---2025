#include <windows.h>
#include <tchar.h>
#include <fcntl.h>
#include <stdio.h>
#include <io.h>
#include <time.h>


#define SHM_NAME TEXT("SHM_PC")           // nome da memoria partilhada
#define MUTEX_NAME TEXT("MUTEX")          // nome do mutex   -> em casa devem pensar numa solução para ter mutex´s distintos de forma a não existir perda de performance 
#define SEM_WRITE_NAME TEXT("SEM_WRITE")  // nome do semaforo de escrita
#define SEM_READ_NAME TEXT("SEM_READ")    // nome do semaforo de leitura
#define REG_PATH TEXT("Software\\TrabSO2") // path do registry para ir buscar maxletras e ritmo
#define BUFFER_SIZE 10

typedef struct _BufferCell {
	unsigned int id; //id do produtor
	unsigned  val; // valor que o produtor gerou
} BufferCell;

typedef struct _SharedMem {
	unsigned int p;   // contador partilhado com o numero de produtores  
	unsigned int c;   // contador partilhado com o numero de consumidores   
	unsigned int wP;  // posicao do buffer circular para a escrita     
	unsigned int rP;  // posicao do buffer circular para a escrita  
	BufferCell buffer[BUFFER_SIZE]; // buffer circular
} SharedMem;

typedef struct _ControlData {
	unsigned int shutdown;  // flag "continua". 0 = continua, 1 = deve terminar
	unsigned int id;        // id do processo  
	unsigned int count;     // contador do numero de vezes  
	HANDLE hMapFile;        // ficheiro de memoria 
	SharedMem* sharedMem;   // memoria partilhada
	HANDLE hMutex;          // mutex - trabalho de casa -> acrescentar os outros 3 mutexes
	HANDLE hWriteSem;       // semaforo "aguarda por items escritos"
	HANDLE hReadSem;        // semaforo "aguarda por posições vazias"
} ControlData;



BOOL readOrCreateRegistryValues(int* maxLetras, int* ritmo) {
	HKEY hKey;
	DWORD dwType = REG_DWORD;
	DWORD dwSize = sizeof(DWORD);
	DWORD val;

	LONG result = RegOpenKeyEx(HKEY_CURRENT_USER, REG_PATH, 0, KEY_READ | KEY_WRITE, &hKey);
	if (result != ERROR_SUCCESS) {
		// Chave não existe, criar
		result = RegCreateKeyEx(HKEY_CURRENT_USER, REG_PATH, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hKey, NULL);
		if (result != ERROR_SUCCESS) {
			_tprintf(TEXT("Erro ao criar chave do Registry (%ld)\n"), result);
			return FALSE;
		}

		//quando crio a chave estes vao ser os valores default para cada par nome/valor:
		// 6 e 3 são os valores default pedidos no enunciado

		val = 6; 
		RegSetValueEx(hKey, TEXT("MAXLETRAS"), 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
		*maxLetras = val;

		val = 3;
		RegSetValueEx(hKey, TEXT("RITMO"), 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
		*ritmo = val;
	}

	else { //se os pares nome/valor  não existirem no registo, mas o registo existe:
		// Ler MAXLETRAS
		if (RegQueryValueEx(hKey, TEXT("MAXLETRAS"), NULL, &dwType, (BYTE*)&val, &dwSize) != ERROR_SUCCESS) {
			val = 6;
			RegSetValueEx(hKey, TEXT("MAXLETRAS"), 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
		}
		*maxLetras = (val > 12) ? 12 : val;

		if (val > 12) {
			val = 12;
			RegSetValueEx(hKey, TEXT("MAXLETRAS"), 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
		}

		// Ler RITMO
		dwSize = sizeof(DWORD);
		if (RegQueryValueEx(hKey, TEXT("RITMO"), NULL, &dwType, (BYTE*)&val, &dwSize) != ERROR_SUCCESS) {
			val = 3;
			RegSetValueEx(hKey, TEXT("RITMO"), 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
		}
		*ritmo = val;
	}

	RegCloseKey(hKey);
	return TRUE;
} 
//concluido 18/4 ^


//função que inicializa a memoria, ...

BOOL initMemAndSync(ControlData* cdata)
{
	//ir buscar os valores ao registry
	int maxLetras = 0, ritmo = 0;
	if (!readOrCreateRegistryValues(&maxLetras, &ritmo)) {
		_tprintf(TEXT("Erro ao ler/criar valores do Registry.\n"));
		return FALSE;
	}
	_tprintf(TEXT("MAXLETRAS: %d | RITMO: %d\n"), maxLetras, ritmo);


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
		0,//USAR 0 SE O FICHEIRO FOR MENOR QUE 4GB
		0,//de onde começamos a mapear
		sizeof(SharedMem)); //tamanho max

	if (cdata->sharedMem == NULL) {
		_tprintf(TEXT("Erro MapVieOf file %d"), GetLastError());
		CloseHandle(cdata->hMapFile);
		return FALSE;
	}

	//inicializa
	if (firstProcess) {
		cdata->sharedMem->p = 0; //nr produtores
		cdata->sharedMem->c = 0; //contador de consumidore
		cdata->sharedMem->wP = 0; //posicao de 0 a buffersize para escrita
		cdata->sharedMem->rP = 0; //posicao de 0 a buffersize para leitura
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

//função thread que vai estar a produzir
DWORD WINAPI produce(LPVOID p) {
	ControlData* cdata = (ControlData*)p;
	BufferCell cell;
	cell.id = cdata->id; //id do produtor, incrementado no main

	while (1) {
		if (cdata->shutdown)
			return 0; //flag para terminar

		WaitForSingleObject(cdata->hWriteSem, INFINITE);//verifico se posso escrever no array, ou seja se há vagas
		WaitForSingleObject(cdata->hMutex, INFINITE);//mexer na memoria, zona critica

		CopyMemory(0,0,0); //escrever na memoria partilhada 

		if (cdata->sharedMem->wP == BUFFER_SIZE)
			cdata->sharedMem->wP = 0;//volta a escrever do principio, caso chegue ao limite

		ReleaseMutex(cdata->hMutex);//fim zona critica
		ReleaseSemaphore(cdata->hReadSem, 1, NULL);//liberto o semaforo de leitura, para avisar o consumidor que existem dados p ler

		//_tprintf(TEXT("P%d produced %d\n"), cell.id, cell.val);

		Sleep(1000);

		cdata->count++;//nr de itens
	}
	return 0;
}

int _tmain(int argc, TCHAR* argv[])
{
	ControlData cdata;
	HANDLE hThread;
	TCHAR command[100];
#ifdef UNICODE
	_setmode(_fileno(stdin), _O_WTEXT);
	_setmode(_fileno(stdout), _O_WTEXT);
	_setmode(_fileno(stderr), _O_WTEXT);
#endif

	cdata.shutdown = 0; //flag
	cdata.count = 0; //numero de itens

	if (!initMemAndSync(&cdata))
	{
		_tprintf(TEXT("Error creating/opening shared memory and synchronization mechanisms.\n"));
		exit(1);
	}

	WaitForSingleObject(cdata.hMutex, INFINITE);

	cdata.id = ++(cdata.sharedMem->p); // incrementa o contador partilhado com o numero de produtor
	
	ReleaseMutex(cdata.hMutex);

	hThread = CreateThread(NULL, 0, produce, &cdata, 0, NULL);

	if (hThread == NULL) {
		_tprintf(TEXT("Error creating thread (%d)\n"), GetLastError());
		CloseHandle(cdata.hMapFile);
		CloseHandle(cdata.hMutex);
		CloseHandle(cdata.hWriteSem);
		CloseHandle(cdata.hReadSem);
		return 1;
	}
	_tprintf(TEXT("Type in 'exit' to leave.\n"));

	do {
		_getts_s(command, 100);
	} while (_tcscmp(command, TEXT("exit")) != 0);


	//função para saída ordeira


	cdata.shutdown = 1; //altera a flag para que a thread termine
	WaitForSingleObject(hThread, INFINITE);

	CloseHandle(hThread);
	UnmapViewOfFile(cdata.sharedMem);
	CloseHandle(cdata.hMapFile);
	CloseHandle(cdata.hMutex);
	CloseHandle(cdata.hWriteSem);
	CloseHandle(cdata.hReadSem);
	return 0;

}
