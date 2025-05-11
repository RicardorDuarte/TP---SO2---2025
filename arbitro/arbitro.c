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
#define EVENT_NAME TEXT("EVENT")          // nome do evento
#define REG_PATH TEXT("Software\\TrabSO2") // path do registry para ir buscar maxletras e ritmo
#define BUFFER_SIZE 6
#define NUSERS 10

typedef struct _BufferCell {
	TCHAR  letra; // valor que o produtor gerou
} BufferCell;

typedef struct _SharedMem {
	unsigned int c;   //    
	unsigned int wP;  // posicao do buffer circular para a escrita     
	unsigned int rP;  // posicao do buffer circular para a escrita  
	BufferCell buffer[BUFFER_SIZE]; // buffer circular
	char users[NUSERS][25]; // array de strings para os nomes dos jogadores
	int nusers;
} SharedMem;

typedef struct _ControlData {
	unsigned int shutdown;  // flag "continua". 0 = continua, 1 = deve terminar
	unsigned int id;        // id do processo  (necessario para tp?)
	unsigned int count;     // contador do numero de vezes (necessario para tp?)  
	HANDLE hMapFile;        // ficheiro de memoria 
	SharedMem* sharedMem;   // memoria partilhada
	HANDLE hMutex;          // mutex 
	HANDLE hEvent;          // evento para leitura sincronizada
	HANDLE hPipe[NUSERS];   // array de handles para os pipes de cada jogador
	unsigned int nPipes; // maximo de letras
} ControlData;

typedef struct _PipeMsg {
	HANDLE hPipe; //necessario?
	TCHAR buff[256];
	BOOL isUsernameInvalid;
	TCHAR username[26];
} PipeMsg;


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
		cdata->sharedMem->c = 0; //contador de clientes logados
		cdata->sharedMem->wP = 0; //posicao de 0 a buffersize para escrita
		cdata->sharedMem->rP = 0; //posicao de 0 a buffersize para leitura

		for (int i = 0; i < BUFFER_SIZE; i++) {
			cdata->sharedMem->buffer[i].letra = _T('_'); // inserir '_' em espaços em branco
		}
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

	HANDLE hEvent;
	hEvent = CreateEvent(NULL, TRUE, FALSE, EVENT_NAME);
	if (hEvent == NULL) {
		_tprintf_s(_T("Erro ao criar evento: %ld\n"), GetLastError());
		CloseHandle(cdata->hMutex);
		return FALSE;
	}
	cdata->hEvent = hEvent;


	return TRUE;
}

//função thread que vai estar a produzir
DWORD WINAPI enviaLetras(LPVOID p) {
	ControlData* cdata = (ControlData*)p;
	BufferCell cell;
	static TCHAR letras[] = _T("abcdefghijklmnopqrstuvwxyz");

	while (1) {
		if (cdata->shutdown)
			return 0; //flag para terminar

		SetEvent(cdata->hEvent); // sinaliza que o consumidor pode ler
		WaitForSingleObject(cdata->hMutex, INFINITE);//mexer na memoria, zona critica
		
		unsigned indice = rand() % 25;
		cell.letra = letras[indice];

		int pos = cdata->sharedMem->wP;


		CopyMemory(&(cdata->sharedMem->buffer[(cdata->sharedMem->wP)++]), &cell, sizeof(BufferCell));
		if (cdata->sharedMem->wP == BUFFER_SIZE)
			cdata->sharedMem->wP = 0;//volta a escrever do principio, caso chegue ao limite
		
		ReleaseMutex(cdata->hMutex);//fim zona critica

		ResetEvent(cdata->hEvent); // reseta o evento para que o consumidor possa ler


		// lançar as letras no ritmo certo:
		LONG result;
		HKEY hKey;
		DWORD size = sizeof(DWORD);
		unsigned int val;
		result = RegOpenKeyEx(HKEY_CURRENT_USER, TEXT("Software\\TrabSO2"), 0, KEY_READ | KEY_WRITE, &hKey);
		if (result != ERROR_SUCCESS) {
			printf("Erro ao abrir chave do registry (%ld)\n", result);
			return -1;
		}

		// Ler valor atual de RITMO
		result = RegQueryValueEx(hKey, TEXT("RITMO"), NULL, NULL, (LPBYTE)&val, &size);
		if (result != ERROR_SUCCESS) {
			_tprintf(_T("Erro ao ler RITMO, usando valor por omissão (3)\n"));
			val = 3;
		}

		Sleep(1000 * val); // espera o tempo definido no registry

		WaitForSingleObject(cdata->hMutex, INFINITE);
		(cdata->sharedMem->nusers)++;
		ReleaseMutex(cdata->hMutex);

	}
	return 0;
}



void tratarComando(const TCHAR* comando, LPVOID lpParam) {
	ControlData* cdata = (ControlData*)lpParam;
	HKEY hKey;
	DWORD size = sizeof(DWORD);
	LONG result;
	unsigned int val;

	_tprintf(_T("Comando recebido: %s\n"), comando);
	if (_tcscmp(comando, _T("exit")) == 0) {
		WaitForSingleObject(cdata->hMutex, INFINITE);
		cdata->shutdown = 1; 
		ReleaseMutex(cdata->hMutex);
		_tprintf(_T("A terminar...\n"));
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
	return;
}


DWORD WINAPI comunica(LPVOID tdata) {
	ControlData* cdata = (ControlData*)tdata;
	//ainda nao sei o que mandar pelo pipe, melhor coisa seria estruturas como em SO
	return 0;
}


void saidaordeira(ControlData* cdata, HANDLE hTread, HANDLE mutexGlobal) {
	//função para saída ordeira
	WaitForSingleObject(cdata->hMutex, INFINITE);
	cdata->shutdown = 1; //altera a flag para que a thread termine
	ReleaseMutex(cdata->hMutex);

	//fechar os pipes
	_tprintf(_T("a tentar terminar\n"));
	WaitForSingleObject(hTread, INFINITE);
	for (int i = 0; i < cdata->sharedMem->nusers; i++) {
		CloseHandle(cdata->hPipe[i]);
	}
	//fechar os handles para terminar
	CloseHandle(cdata->hMapFile);
	CloseHandle(cdata->hMutex);
	//fechar o mutex de instancia unica
	ReleaseMutex(mutexGlobal);
	CloseHandle(mutexGlobal);
}


DWORD WINAPI keyboardThread(LPVOID p) {
	ControlData* cdata = (ControlData*)p;
	TCHAR command[100];

	do {
		_getts_s(command, 100);
		tratarComando(command, (LPVOID) & cdata);
		_tprintf(_T("shutdown (thread): %d\n"), cdata->shutdown);
	} while (1);

	return NULL;
}

BOOL isUsernameInvalid(const TCHAR* username) {
	
}





int _tmain(int argc, TCHAR* argv[])
{
	LPTSTR PIPE_NAME = _T("\\\\.\\pipe\\xpto");
	ControlData cdata;
	HANDLE hThread = NULL,hThrTeclado = NULL,hPipe = NULL;
	TCHAR command[100];
	PipeMsg pidata;

#ifdef UNICODE
	_setmode(_fileno(stdin), _O_WTEXT);
	_setmode(_fileno(stdout), _O_WTEXT);
	_setmode(_fileno(stderr), _O_WTEXT);
#endif

	cdata.shutdown = 0; //flag
	cdata.count = 0; //numero de 
	cdata.nPipes = 0; //numero de pipes



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

	// Imprime o a memória partilhada inicial, que deve ser apenas " _ "
	for (int i = 0; i < BUFFER_SIZE; i++) {
		_tprintf(TEXT("%c\t"), cdata.sharedMem->buffer[i].letra);
	}
	_tprintf(TEXT("\n"));

	
	_tprintf(TEXT("Type in 'exit' to leave.\n"));

	/*
	hThread = CreateThread(NULL, 0, enviaLetras, &cdata, 0, NULL);

	if (hThread == NULL) {
		_tprintf(TEXT("Error creating thread (%d)\n"), GetLastError());
		saidaordeira(&cdata, hThread, hSingle_instance);
		return 1;
	}
	*/

	hThrTeclado = CreateThread(NULL, 0, keyboardThread, &cdata, 0, NULL);
	if (hThrTeclado == NULL) {
		_tprintf(TEXT("Error creating keyboard thread (%d)\n"), GetLastError());
		saidaordeira(&cdata, hThread, hSingle_instance);
		return 1;
	}


	
	do {
		BOOL fConnected = FALSE;
		unsigned int i = 0;
		hPipe = CreateNamedPipe(
			PIPE_NAME,             // nome do pipe 
			PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,       // acesso read/write 
			PIPE_TYPE_MESSAGE |       // tipo de pipe = message
			PIPE_READMODE_MESSAGE |   // com modo message-read e
			PIPE_WAIT,                // bloqueante 
			PIPE_UNLIMITED_INSTANCES, // max. instancias (255)
			sizeof(TCHAR),                  // tam buffer output
			sizeof(TCHAR),                  // tam buffer input 
			50,                     // time-out p/ cliente 5k milisegundos (0->default=50)
			NULL);                    // atributos seguran�a default

		if (hPipe == INVALID_HANDLE_VALUE) {
			_tprintf(_T("[ERRO] Criar pipe! Error: %d\n"), GetLastError());
			saidaordeira(&cdata, hThread, hSingle_instance);
			return -1;
		}

		_tprintf(_T("À espera de conexão de um user. . .\n"));

		fConnected = ConnectNamedPipe(hPipe, NULL) ?
			TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

		if (hPipe == INVALID_HANDLE_VALUE) {
			_tprintf(_T("[ERRO] Criar pipe! Error: %d\n"), GetLastError());
			saidaordeira(&cdata, hThread, hSingle_instance);
			return -1;
		}
	
		if (fConnected) {
			PipeMsg receivedMsg;
			DWORD bytesRead;
			DWORD bytesWritten;
			PipeMsg responseMsg;
			BOOL usernameExists = FALSE;

			ZeroMemory(&receivedMsg, sizeof(PipeMsg));
			ZeroMemory(&responseMsg, sizeof(PipeMsg));

			if (ReadFile(hPipe, &receivedMsg, sizeof(PipeMsg), &bytesRead, NULL) && bytesRead == sizeof(PipeMsg)) {
				ZeroMemory(&responseMsg, sizeof(PipeMsg));

				WaitForSingleObject(cdata.hMutex, INFINITE);

				// existe?
				for (int i = 0; i < cdata.sharedMem->nusers; i++) {
					if (_tcscmp(cdata.sharedMem->users[i], receivedMsg.username) == 0) {
						usernameExists = TRUE;
						break;
					}
				}

				if (!usernameExists && cdata.sharedMem->nusers < NUSERS) {
					// adiciona user
					_tcscpy_s(cdata.sharedMem->users[cdata.sharedMem->nusers], 26, receivedMsg.username);
					cdata.sharedMem->nusers++;

					// resposta posivita
					responseMsg.isUsernameInvalid = FALSE;
					_tprintf(_T("Novo user registado: %s\n"), receivedMsg.username);
				}
				else {
					// resposta negativa
					responseMsg.isUsernameInvalid = TRUE;
					_tprintf(_T("Login recusado para: %s\n"), receivedMsg.username);
				}

				ReleaseMutex(cdata.hMutex);

				WriteFile(hPipe, &responseMsg, sizeof(PipeMsg), &bytesWritten, NULL);

				if (!responseMsg.isUsernameInvalid) {
					DisconnectNamedPipe(hPipe);
					CloseHandle(hPipe);
					continue;
				}
			}

			_tprintf(_T("Ligação a leitor bem sucedida!\n"));

			WaitForSingleObject(cdata.hMutex, INFINITE);
			//adicionar o pipe à lista de pipes
			cdata.hPipe[cdata.nPipes] = hPipe;
			cdata.count++;
			cdata.nPipes++;
			ReleaseMutex(cdata.hMutex);
		}	


		_tprintf(_T("shutdown %d\n"), cdata.shutdown);
	} while (cdata.shutdown == 0);


	WaitForSingleObject(hThrTeclado, INFINITE);
	saidaordeira(&cdata, hThread, hSingle_instance);

	_tprintf(_T("Saída ordeira bem sucedida\n"));

	return 0;
}
