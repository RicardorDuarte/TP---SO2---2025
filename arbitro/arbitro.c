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
#define NUSERS 10
#define PIPE_NAME _T("\\\\.\\pipe\\teste")

typedef struct _BufferCell {
	unsigned int ttl; //tempo de vida da letra
	TCHAR  letra; // valor que o produtor gerou
	unsigned val;
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
	unsigned int id;        // id do processo  (necessario para tp?)
	unsigned int count;     // contador do numero de vezes (necessario para tp?)  
	HANDLE hMapFile;        // ficheiro de memoria 
	SharedMem* sharedMem;   // memoria partilhada
	HANDLE hMutex;          // mutex 
	HANDLE hWriteSem;       // semaforo "aguarda por items escritos"
	HANDLE hReadSem;        // semaforo "aguarda por posições vazias"
	HANDLE hPipe[NUSERS]; // array de handles para os pipes de cada jogador
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

	else { //registo existe:
		// Ler MAXLETRAS 
		if (RegQueryValueEx(hKey, TEXT("MAXLETRAS"), NULL, &dwType, (BYTE*)&val, &dwSize) != ERROR_SUCCESS) { //se não consegue ler é pq o par não existe
			val = 6;
			RegSetValueEx(hKey, TEXT("MAXLETRAS"), 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD)); //então cria-o
		}
		*maxLetras = (val > 12) ? 12 : val;

		if (val > 12) {
			val = 12;
			RegSetValueEx(hKey, TEXT("MAXLETRAS"), 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD));
		}

		// Ler RITMO
		dwSize = sizeof(DWORD);
		if (RegQueryValueEx(hKey, TEXT("RITMO"), NULL, &dwType, (BYTE*)&val, &dwSize) != ERROR_SUCCESS) { //se não consegue ler é pq o par não existe
			val = 3;
			RegSetValueEx(hKey, TEXT("RITMO"), 0, REG_DWORD, (BYTE*)&val, sizeof(DWORD)); //então cria-o
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
	static TCHAR letras[] = _T("abcdefghijklmnopqrstuvwxyz");

	while (1) {
		if (cdata->shutdown)
			return 0; //flag para terminar

		WaitForSingleObject(cdata->hWriteSem, INFINITE);//verifico se posso escrever no array, ou seja se há vagas
		WaitForSingleObject(cdata->hMutex, INFINITE);//mexer na memoria, zona critica
		unsigned indice = rand() % 25;
		cell.letra = letras[indice];

		int pos = cdata->sharedMem->wP;

		CopyMemory(&(cdata->sharedMem->buffer[(cdata->sharedMem->wP)++]), &cell, sizeof(BufferCell));
		if (cdata->sharedMem->wP == BUFFER_SIZE)
			cdata->sharedMem->wP = 0;//volta a escrever do principio, caso chegue ao limite

		_tprintf(TEXT("Arbitro gerou letra: %lc, memoria partilhada: %lc\n"), cell.letra,
			cdata->sharedMem->buffer[pos].letra);


		ReleaseMutex(cdata->hMutex);//fim zona critica
		ReleaseSemaphore(cdata->hReadSem, 1, NULL);//liberto o semaforo de leitura, para avisar o consumidor que existem dados p ler


		// lançar as letras no ritmo certo:
		LONG result;
		HKEY hKey;
		DWORD size = sizeof(DWORD);
		unsigned int val;
		result = RegOpenKeyEx(HKEY_CURRENT_USER, TEXT("Software\\TrabSO2"), 0, KEY_READ | KEY_WRITE, &hKey);
		if (result != ERROR_SUCCESS) {
			printf("Erro ao abrir chave do registry (%ld)\n", result);
			return;
		}

		// Ler valor atual de RITMO
		result = RegQueryValueEx(hKey, TEXT("RITMO"), NULL, NULL, (LPBYTE)&val, &size);
		if (result != ERROR_SUCCESS) {
			_tprintf(_T("Erro ao ler RITMO, usando valor por omissão (3)\n"));
			val = 3;
		}

		Sleep(1000 * val); // espera o tempo definido no registry
		cdata->count++;
	}
	return 0;
}

void tratarComando(const TCHAR* comando) {
	HKEY hKey;
	DWORD size = sizeof(DWORD);
	LONG result;
	unsigned int val;


	if (_tcscmp(comando, _T("exit")) == 0) {
		return;
	}

	// Abrir chave
	result = RegOpenKeyEx(HKEY_CURRENT_USER, TEXT("Software\\TrabSO2"), 0, KEY_READ | KEY_WRITE, &hKey);
	if (result != ERROR_SUCCESS) {
		printf("Erro ao abrir chave do registry (%ld)\n", result);
		return;
	}

	// Ler valor atual de RITMO
	result = RegQueryValueEx(hKey, TEXT("RITMO"), NULL, NULL, (LPBYTE)&val, &size);
	if (result != ERROR_SUCCESS) {
		_tprintf(_T("Erro ao ler RITMO, usando valor por omissão (3)\n"));
		val = 3;
	}
	

	if (_tcscmp(comando, _T("acelerar")) == 0) {
		if (val > 1) {
			val--;
		}
		else {
			_tprintf(_T("RITMO já está no mínimo (1 segundo)\n"));
			val = 1;
		}
		_tprintf(_T("RITMO acelerado para %lu segundos\n"), val);
	}
	else if (_tcscmp(comando, _T("travar")) == 0) {
		val++;
		_tprintf(_T("RITMO travado para %lu segundos\n"), val);
	}

	// Atualizar no registry
	result = RegSetValueEx(hKey, TEXT("RITMO"), 0, REG_DWORD, (BYTE*)&val, size);
	if (result != ERROR_SUCCESS) {
		_tprintf(_T("Erro ao atualizar RITMO no registry\n"));
	}

	


	RegCloseKey(hKey);
}


DWORD WINAPI comunica(LPVOID tdata) {
	ControlData* cdata = (ControlData*)tdata;
	//criar pipe para cada jogador
	//ainda nao sei o que mandar pelo pipe, melhor coisa seria estruturas como em SO
}


void saidaordeira(ControlData* cdata, HANDLE hTread, HANDLE mutexGlobal) {
	//função para saída ordeira
	cdata->shutdown = 1; //altera a flag para que a thread termine
	//fechar os pipes
	_tprintf(_T("a tentar terminar\n"));
	WaitForSingleObject(hTread, INFINITE);
	for (int i = 0; i < cdata->sharedMem->nusers; i++) {
		CloseHandle(cdata->hPipe[i]);
	}
	//fechar os handles para terminar
	CloseHandle(cdata->hMapFile);
	CloseHandle(cdata->hMutex);
	CloseHandle(cdata->hWriteSem);
	CloseHandle(cdata->hReadSem);
	//fechar o mutex de instancia unica
	ReleaseMutex(mutexGlobal);
	CloseHandle(mutexGlobal);
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
	/*
		if (argc < 2) {
			_tprintf(TEXT("Erro: Nenhum username fornecido.\n"));
		}
		char *username = argv[1];
		strncpy_s(cdata.sharedMem->users[cdata.sharedMem->nusers], NUSERS,username, _TRUNCATE);
		cdata.sharedMem->users[cdata.sharedMem->nusers]= '\0'; // Garante terminação nula
		cdata.sharedMem->nusers++;
	*/
	cdata.shutdown = 0; //flag
	cdata.count = 0; //numero de itens

	//Como este mutex é global nem com outro user do windows seria possivel iniciar duas instâncias do arbitro
	HANDLE hSingle_instance = CreateMutex(NULL, FALSE, TEXT("Global\\ARBITRO_UNICO"));
	if (hSingle_instance == NULL) {
		_tprintf(TEXT("Erro a criar mutex de instancia unica (%d)\n"), GetLastError());
		return 1;
	}

	// Verificar se já existe um arbitro a correr
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		_tprintf(TEXT("Já existe uma instância do árbitro a correr.\n"));
		CloseHandle(hSingle_instance);
		return 1;
	}


	if (!initMemAndSync(&cdata))
	{
		_tprintf(TEXT("Error creating/opening shared memory and synchronization mechanisms.\n"));
		exit(1);
	}

	WaitForSingleObject(cdata.hMutex, INFINITE);

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
		_getts_s(command, 100); //falta sincronização
		if (_tcscmp(command, _T("exit")) != 0)
			tratarComando(command);
	} while (_tcscmp(command, TEXT("exit")) != 0);


	//função para saída ordeira

	saidaordeira(&cdata, hThread, hSingle_instance);
	
	_tprintf(_T("Saída ordeira bem sucedida\n"));
	
	return 0;

}
