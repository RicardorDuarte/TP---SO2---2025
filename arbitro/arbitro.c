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

	while (!cdata->shutdown) {
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
	DWORD bytesWritten;
	HKEY hKey;
	DWORD size = sizeof(DWORD);
	LONG result;
	TCHAR cmdPrincipal[256] = { 0 };
	TCHAR cmdSec[256] = { 0 };
	unsigned int val,n=0;
	PipeMsg msg = { 0 };


	int args = _stscanf_s(comando, _T("%255s %255s"), cmdPrincipal, 256, cmdSec, 256);

	_tprintf(_T("Comando completo: %s\n"), comando);
	_tprintf(_T("Partes encontradas: %d\n"), args);

	if (args >= 1) {
		_tprintf(_T("primeiro token: %s\n"), cmdPrincipal);

	}

	if (args >= 2) {
		_tprintf(_T("Segundo token: %s\n"), cmdSec);
	}

	if (_tcscmp(cmdPrincipal, _T("encerrar")) == 0) {
		WaitForSingleObject(cdata->hMutex, INFINITE);
		cdata->shutdown = 1;
		ReleaseMutex(cdata->hMutex);
		_tprintf(_T("A terminar...\n"));
		return ;
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


	if (_tcscmp(cmdPrincipal, _T("acelerar")) == 0) {
		if (val > 1) {
			val--;
		}
		else {
			_tprintf(_T("RITMO já está no mínimo (1 segundo)\n"));
			val = 1;
		}
		_tprintf(_T("RITMO acelerado para %lu segundos\n"), val);
	}
	else if (_tcscmp(cmdPrincipal, _T("travar")) == 0) {
		val++;
		_tprintf(_T("RITMO travado para %lu segundos\n"), val);
	}

	// Atualizar no registry
	result = RegSetValueEx(hKey, TEXT("RITMO"), 0, REG_DWORD, (BYTE*)&val, size);
	if (result != ERROR_SUCCESS) {
		_tprintf(_T("Erro ao atualizar RITMO no registry\n"));
	}

	if (_tcscmp(cmdPrincipal, _T("listar")) == 0) {
		n = cdata->sharedMem->nusers;
		if (n == 0) {
			_tprintf(TEXT("Nenhum user!\n"));
			return;
		}
		_tprintf(_T("Lista de jogadores:\n"));
		for (int i = 0; i < cdata->sharedMem->nusers; i++) {
			_tprintf(_T("%s\n"), cdata->sharedMem->users[i]);
		}
	}

	if (_tcscmp(cmdPrincipal, _T("excluir")) == 0) {

		for (n = 0; n < cdata->sharedMem->nusers; n++) {
			if (_tcscmp(cmdSec, cdata->sharedMem->users[n]) == 0)
				break;
		}
		//enviar uma mensagem a dizer para ele fechar
		
		if (n < cdata->sharedMem->nusers) {
			_tcscpy_s(msg.username, 26, cdata->sharedMem->users[n]);
			_tcscpy_s(msg.buff, 256, _T("close")); // comando especial para o cliente sair
			msg.isUsernameInvalid = FALSE;

			WriteFile(cdata->hPipe[n], &msg, sizeof(PipeMsg), &bytesWritten, NULL);

			// Feche o pipe
			CloseHandle(cdata->hPipe[n]);

			// remova o utilizador do array e ajuste nusers
			for (int i = n; i < cdata->sharedMem->nusers - 1; i++) {
				_tcscpy_s(cdata->sharedMem->users[i], 26, cdata->sharedMem->users[i + 1]);
				cdata->hPipe[i] = cdata->hPipe[i + 1];
				cdata->sharedMem->pontuacao[i] = cdata->sharedMem->pontuacao[i + 1];
			}
			cdata->sharedMem->nusers--;
			cdata->nPipes--;
		}

		for (n = 0; n < cdata->sharedMem->nusers; n++) {
			_tcscpy_s(msg.username, 26, cdata->sharedMem->users[n]);
			_stprintf_s(msg.buff, 256, _T("utilizador %s foi encerrado\n"), cmdSec);
			msg.isUsernameInvalid = FALSE;
			WriteFile(cdata->hPipe[n], &msg, sizeof(PipeMsg), &bytesWritten, NULL);
		}

		return;
	}


	RegCloseKey(hKey);
	return 0;
}


DWORD WINAPI comunica(LPVOID tdata) {
	ControlData* cdata = (ControlData*)tdata;
	HANDLE hPipe = cdata->hPipe[cdata->nPipes - 1]; 
	PipeMsg receivedMsg;
	PipeMsg responseMsg;
	DWORD bytesRead, bytesWritten;
	BOOL usernameExists = FALSE;
	BOOL fSuccess;


	// Loop principal de comunicação
	while (!cdata->shutdown) {
		// Limpa as mensagens para nova comunicação
		ZeroMemory(&receivedMsg, sizeof(PipeMsg));
		ZeroMemory(&responseMsg, sizeof(PipeMsg));

		// Lê mensagem do cliente
		fSuccess = ReadFile(hPipe, &receivedMsg, sizeof(PipeMsg), &bytesRead, NULL);

		_tprintf(_T("Mensagem recebida de %s: %s\n"), receivedMsg.username, receivedMsg.buff);

		if (!fSuccess || bytesRead != sizeof(PipeMsg)) {
			DWORD err = GetLastError();
			if (err == ERROR_BROKEN_PIPE) {
				_tprintf(_T("Cliente %s desconectou\n"), receivedMsg.username);
			}
			else {
				_tprintf(_T("Erro ao ler mensagem (%d)\n"), err);
			}
			break;
		}


		// Processa comando
		if (_tcscmp(receivedMsg.buff, _T(":sair")) == 0) {
			_tprintf(_T("Cliente %s solicitou saída\n"), receivedMsg.username);
			WaitForSingleObject(cdata->hMutex, INFINITE);

			// Remove o usuário do array
			for (int i = 0; i < cdata->sharedMem->nusers; i++) {
				if (_tcscmp(cdata->sharedMem->users[i], receivedMsg.username) == 0) {
					// Move os usuários seguintes para preencher o espaço
					for (int j = i; j < cdata->sharedMem->nusers - 1; j++) {
						_tcscpy_s(cdata->sharedMem->users[j], 25,
							cdata->sharedMem->users[j + 1]);
						cdata->sharedMem->pontuacao[j] = cdata->sharedMem->pontuacao[j + 1];
					}
					cdata->sharedMem->nusers--;
					break;
				}
			}

			// Libera o mutex
			ReleaseMutex(cdata->hMutex);

			break;
		}

		if (_tcscmp(receivedMsg.buff, _T(":jogs")) == 0) {
			_tprintf(_T("Cliente %s solicitou a lista de jogadores\n"), receivedMsg.username);
			WaitForSingleObject(cdata->hMutex, INFINITE);

			// Prepara a resposta
			_tcscpy_s(responseMsg.username, 26, receivedMsg.username);
			responseMsg.isUsernameInvalid = FALSE;

			// Constrói a lista de jogadores
			_stprintf_s(responseMsg.buff, 256, _T("\nJogadores conectados (%d):\n"), cdata->sharedMem->nusers);

			for (int i = 0; i < cdata->sharedMem->nusers; i++) {
				// Adiciona cada jogador à mensagem (verificando espaço)
					_tcscat_s(responseMsg.buff, 256, cdata->sharedMem->users[i]);
					if (i < cdata->sharedMem->nusers - 1)
						_tcscat_s(responseMsg.buff, 256, _T("-"));
			}
			_tcscat_s(responseMsg.buff, 256, _T("\n"));

			// Envia uma única mensagem completa
			fSuccess = WriteFile(hPipe, &responseMsg, sizeof(PipeMsg), &bytesWritten, NULL);
			if (!fSuccess || bytesWritten != sizeof(PipeMsg)) {
				_tprintf(_T("Erro ao enviar lista de jogadores (%d)\n"), GetLastError());
			}
			else {
				_tprintf(_T("Lista de jogadores enviada com sucesso\n"));
			}

			ReleaseMutex(cdata->hMutex);
			continue;
		}

		if (_tcscmp(receivedMsg.buff, _T(":pont")) == 0) {
			_tprintf(_T("Cliente %s solicitou a lista de pontuações\n"), receivedMsg.username);
			WaitForSingleObject(cdata->hMutex, INFINITE);

			// Prepara a resposta
			_tcscpy_s(responseMsg.username, 26, receivedMsg.username);
			responseMsg.isUsernameInvalid = FALSE;

			// Constrói a lista de pontuações
			_stprintf_s(responseMsg.buff, 256, _T("\nPontuações dos jogadores:\n"));
			for (int i = 0; i < cdata->sharedMem->nusers; i++) {
				TCHAR temp[64];
				_stprintf_s(temp, 64, _T("%s: %d"), cdata->sharedMem->users[i], cdata->sharedMem->pontuacao[i]);
				_tcscat_s(responseMsg.buff, 256, temp);
				if (i < cdata->sharedMem->nusers - 1)
					_tcscat_s(responseMsg.buff, 256, _T(" - "));
			}
			_tcscat_s(responseMsg.buff, 256, _T("\n"));

			// Envia a mensagem completa
			fSuccess = WriteFile(hPipe, &responseMsg, sizeof(PipeMsg), &bytesWritten, NULL);
			if (!fSuccess || bytesWritten != sizeof(PipeMsg)) {
				_tprintf(_T("Erro ao enviar lista de pontuações (%d)\n"), GetLastError());
			}
			else {
				_tprintf(_T("Lista de pontuações enviada com sucesso\n"));
			}

			ReleaseMutex(cdata->hMutex);
			continue;
		}


		// Prepara resposta
		_tcscpy_s(responseMsg.username, 26, receivedMsg.username);
		responseMsg.isUsernameInvalid = FALSE;

		// Envia resposta
		fSuccess = WriteFile(hPipe, &responseMsg, sizeof(PipeMsg), &bytesWritten, NULL);
		if (!fSuccess || bytesWritten != sizeof(PipeMsg)) {
			_tprintf(_T("Erro ao enviar resposta (%d)\n"), GetLastError());
			break;
		}
	}

	
	// Fecha conexão

	_tprintf(_T("Conexão com %s encerrada\n"), receivedMsg.username);
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
		if (_getts_s(command, 100) == NULL) {
			_tprintf(_T("Error reading input or EOF reached\n"));
			break;
		}

		// Remove the & since cdata is already a pointer
		tratarComando(command, cdata);
	} while (cdata->shutdown == 0);

	return 0;  // Return 0 instead of NULL for DWORD
}

BOOL isUsernameInvalid(const TCHAR* username) {

}



int _tmain(int argc, TCHAR* argv[])
{
	LPTSTR PIPE_NAME = _T("\\\\.\\pipe\\xpto");
	ControlData cdata = { 0 };
	HANDLE hThread = NULL, hThrTeclado = NULL, hPipe = NULL, hMSG = NULL;
	TCHAR command[100];
	PipeMsg pidata;
	DWORD cbBytesRead = 0, bytesWritten;

	BOOL fSuccess, usernameExists = FALSE;

#ifdef UNICODE
	_setmode(_fileno(stdin), _O_WTEXT);
	_setmode(_fileno(stdout), _O_WTEXT);
	_setmode(_fileno(stderr), _O_WTEXT);
#endif

	// Initialize control data
	cdata.shutdown = 0;
	cdata.nPipes = 0;

	HANDLE hSingle_instance = CreateMutex(NULL, FALSE, TEXT("Global\\ARBITRO_UNICO"));
	if (hSingle_instance == NULL) {
		_tprintf(TEXT("Erro a criar mutex de instancia unica (%d)\n"), GetLastError());
		return 1;
	}

	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		_tprintf(TEXT("Já existe uma instância do árbitro a correr.\n"));
		CloseHandle(hSingle_instance);
		return 1;
	}

	if (!initMemAndSync(&cdata)) {
		_tprintf(TEXT("Error creating/opening shared memory\n"));
		CloseHandle(hSingle_instance);
		return 1;
	}

	// Print initial shared memory state
	for (int i = 0; i < BUFFER_SIZE; i++) {
		_tprintf(TEXT("%c\t"), cdata.sharedMem->buffer[i].letra);
	}
	_tprintf(TEXT("\n"));


	hThrTeclado = CreateThread(NULL, 0, keyboardThread, &cdata, 0, NULL);
	if (hThrTeclado == NULL) {
		_tprintf(TEXT("Error creating keyboard thread (%d)\n"), GetLastError());
		saidaordeira(&cdata, hThread, hSingle_instance);
		return 1;
	}

	
	_tprintf(_T("À espera de conexão de um user...\n"));

	// MAIN SERVER LOOP - FIXED VERSION
	do {

		hPipe = CreateNamedPipe(
			PIPE_NAME,
			PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
			PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
			PIPE_UNLIMITED_INSTANCES,
			sizeof(PipeMsg),
			sizeof(PipeMsg),
			0,
			NULL);

		if (hPipe == INVALID_HANDLE_VALUE) {
			_tprintf(_T("[ERRO] Criar pipe! Error: %d\n"), GetLastError());
			continue;
		}
		
		fSuccess = ConnectNamedPipe(hPipe, NULL);



		if (fSuccess ) {
			OVERLAPPED OverlRd = { 0 };
			HANDLE ReadReady;
			
			_tprintf(_T("Cliente conectado\n"));
			ReadReady = CreateEvent(
				NULL,   // default security attributes
				TRUE,   // manual-reset event
				FALSE,  // initial state is nonsignaled
				NULL);  // no name

			WaitForSingleObject(cdata.hMutex, INFINITE);
			cdata.hPipe[cdata.nPipes] = hPipe;
			cdata.nPipes++;
			ReleaseMutex(cdata.hMutex);

			_tprintf(_T("Thread de comunicação iniciada para pipe (handle: %p)\n"), hPipe);
			WaitForSingleObject(cdata.hMutex, INFINITE);
			if (cdata.nPipes == 0) {
				ReleaseMutex(cdata.hMutex);
				_tprintf(_T("Erro: Nenhum pipe disponível\n"));
				return 1;
			}

			hPipe = cdata.hPipe[cdata.nPipes - 1];
			ReleaseMutex(cdata.hMutex);
			OverlRd.hEvent = ReadReady;


			ResetEvent(ReadReady);  // não assinalado

			// Recebe o primeiro pedido (login)
			fSuccess = ReadFile(hPipe, &pidata, sizeof(PipeMsg), &cbBytesRead, NULL);
			WaitForSingleObject(ReadReady, 400); 
			//GetOverlappedResult(hPipe, &OverlRd, &cbBytesRead, FALSE);  // sem WAIT


			if (!fSuccess || cbBytesRead != sizeof(PipeMsg)) {
				_tprintf(_T("Erro ao ler mensagem inicial (%d)\n"), GetLastError());
				DisconnectNamedPipe(hPipe);
				CloseHandle(hPipe);
				
			}

			WaitForSingleObject(cdata.hMutex, INFINITE);

			// Verifica se username já existe
			for (int i = 0; i < cdata.sharedMem->nusers; i++) {
				if (_tcscmp(cdata.sharedMem->users[i], pidata.username) == 0) {
					usernameExists = TRUE;
					break;
				}
			}

			if (!usernameExists && cdata.sharedMem->nusers < NUSERS) {
				_tcscpy_s(cdata.sharedMem->users[cdata.sharedMem->nusers], 26, pidata.username);
				cdata.sharedMem->nusers++;
				cdata.sharedMem->pontuacao[cdata.sharedMem->nusers] = 0;
				pidata.isUsernameInvalid = FALSE;
				_tprintf(_T("Novo user registado: %s\n"), pidata.username);
			}
			else {
				pidata.isUsernameInvalid = TRUE;
				_tprintf(_T("Login recusado para: %s\n"), pidata.username);
			}

			ReleaseMutex(cdata.hMutex);

			// Envia resposta do login
			fSuccess = WriteFile(hPipe, &pidata, sizeof(PipeMsg), &bytesWritten, &OverlRd);
			if (!fSuccess || bytesWritten != sizeof(PipeMsg)) {
				_tprintf(_T("Erro ao enviar resposta de login (%d)\n"), GetLastError());
				DisconnectNamedPipe(hPipe);
				CloseHandle(hPipe);
				return 1;
			}

			// Se inválido, termina aqui
			if (pidata.isUsernameInvalid) {
				DisconnectNamedPipe(hPipe);
				CloseHandle(hPipe);
				return 0;
			}

			_tprintf(_T("Ligação com %s estabelecida com sucesso!\n"), pidata.username);

			hMSG = CreateThread(NULL, 0, comunica, &cdata, 0, NULL);
			if (hThrTeclado == NULL) {
				_tprintf(TEXT("Error creating keyboard thread (%d)\n"), GetLastError());
				saidaordeira(&cdata, hThread, hSingle_instance);
				return 1;
			}


			WaitForSingleObject(cdata.hMutex, INFINITE);
			BOOL shouldExit = (cdata.shutdown == 1);
			ReleaseMutex(cdata.hMutex);

			if (shouldExit) {
				_tprintf(_T("Shutdown signal received, exiting...\n"));
				break;
			}

			Sleep(100); // Pequena pausa para evitar consumo excessivo de CPU
		}

	}while (cdata.shutdown == 0);
	// Cleanup
	WaitForSingleObject(hThrTeclado, INFINITE);
	saidaordeira(&cdata, hThread, hSingle_instance);
	_tprintf(_T("Saída ordeira bem sucedida\n"));

	return 0;
}
