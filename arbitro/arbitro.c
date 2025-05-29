#include <windows.h>
#include <tchar.h>
#include <fcntl.h>
#include <stdio.h>
#include <io.h>
#include <time.h>


#define SHM_NAME TEXT("SHM_PC")           // nome da memoria partilhada
#define MUTEX_NAME TEXT("MUTEX")          // nome do mutex  
#define SEM_WRITE_NAME TEXT("SEM_WRITE")  // nome do semaforo de escrita
#define SEM_READ_NAME TEXT("SEM_READ")    // nome do semaforo de leitura
#define EVENT_NAME TEXT("EVENT")          // nome do evento
#define REG_PATH TEXT("Software\\TrabSO2") // path do registry para ir buscar maxletras e ritmo
#define BUFFER_SIZE 6
#define NUSERS 20                       // bug com 3 users, adiciona 254 pontos ao primeiro user logado ???

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
	HANDLE hEvSai;          // evento para desbloquear o gets
	HANDLE hPipe[NUSERS];   // array de handles para os pipes de cada jogador
	unsigned int nPipes; // maximo de letras
	TCHAR dicionario[3][BUFFER_SIZE + 1]; // dicionario de palavras, maximo 3 palavras
} ControlData;

typedef struct _PipeMsg {
	HANDLE hPipe;
	TCHAR buff[256];
	BOOL isUsernameInvalid;
	TCHAR username[26];
} PipeMsg;



unsigned int startgame = 0;


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


	//Criar ou abrir um ficheiro para mapear em memoria
	BOOL firstProcess = FALSE;

	cdata->hMapFile = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, SHM_NAME);//se nao der erro é porque ja existe
	if (cdata->hMapFile == NULL) {//se for null cria
		cdata->hMapFile = CreateFileMapping(
			INVALID_HANDLE_VALUE, //nao ha ficheiro a interligar
			NULL, // seguranca
			PAGE_READWRITE,//permissoes
			0,//tamanho inicial
			sizeof(SharedMem),
			SHM_NAME);
		firstProcess = TRUE;//se foi criada é o primeiro processo
	}
	if (cdata->hMapFile == NULL)
	{
		_tprintf(TEXT("Error: CreateFileMapping (%d)\n"), GetLastError());
		return FALSE;
	}

	//Maps a view of a file mapping into the address space of a calling process
	cdata->sharedMem = (SharedMem*)MapViewOfFile(cdata->hMapFile,//aquilo que pretendemos mapear
		FILE_MAP_ALL_ACCESS, //permissoes de acesso
		0,//
		0,//de onde começamos a mapear
		sizeof(SharedMem)); //tamanho max

	if (cdata->sharedMem == NULL) {
		_tprintf(TEXT("Erro MapVieOf file %d"), GetLastError());
		CloseHandle(cdata->hMapFile);
		return FALSE;
	}

	//inicializa
	if (firstProcess) {
		cdata->sharedMem->nusers = 0; // Garante que começa em 0
		memset(cdata->sharedMem->users, 0, sizeof(cdata->sharedMem->users)); // Limpa o array
		memset(cdata->sharedMem->pontuacao, 0, sizeof(cdata->sharedMem->pontuacao)); // Limpa pontuações
		cdata->sharedMem->c = 0; //contador de clientes logados
		cdata->sharedMem->wP = 0; //posicao de 0 a buffersize para escrita
		cdata->sharedMem->rP = 0; //posicao de 0 a buffersize para leitura

		for (int i = 0; i < BUFFER_SIZE; i++) {
			cdata->sharedMem->buffer[i].letra = _T('_'); // inserir '_' em espaços em branco
		}
		cdata->sharedMem->nusers = 0;
		for (int i = 0; i < NUSERS; i++) {
			cdata->sharedMem->pontuacao[i] = 0; // Inicializa todas as posições
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
	static TCHAR letras[] = _T("maepicroa");
	srand((unsigned int)time(NULL));  // inicializa o gerador com o tempo atual
	unsigned int posVazia = -1;

	while (!cdata->shutdown) {
		if (cdata->shutdown)
			return 0; //terminar


		if (cdata->sharedMem->nusers < 2) {
			_tprintf(_T("Jogadores Insuficientes\n"));
			Sleep(3000); // Espera antes de tentar novamente
			continue;
		}

		SetEvent(cdata->hEvent); // sinaliza que o consumidor pode ler
		WaitForSingleObject(cdata->hMutex, INFINITE);//mexer na memoria, zona critica




		do {
			unsigned indice = rand() % 9;
			cell.letra = letras[indice];
		} while (cell.letra == cdata->sharedMem->buffer[cdata->sharedMem->wP].letra); // garante que a letra é diferente da última escrita

		// 1. Verifica se há espaço vazio ('_') no buffer
		posVazia = -1;
		for (int i = 0; i < BUFFER_SIZE; i++) {
			if (cdata->sharedMem->buffer[i].letra == '_') {
				posVazia = i;
				break;
			}
		}

		// 2. Se há espaço vazio, escreve nele
		if (posVazia != -1) {
			CopyMemory(&(cdata->sharedMem->buffer[posVazia]), &cell, sizeof(BufferCell));
		}
		else {
			// 3. Se não há espaço vazio, escreve na próxima posição (circular)
			CopyMemory(&(cdata->sharedMem->buffer[cdata->sharedMem->wP]), &cell, sizeof(BufferCell));
			cdata->sharedMem->wP = (cdata->sharedMem->wP + 1) % BUFFER_SIZE;
		}

		ReleaseMutex(cdata->hMutex);//fim zona critica
		ResetEvent(cdata->hEvent); // reseta o evento para que o consumidor possa ler

		// Ritmo de escrita
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


	}
	return 0;
}

BOOL LancaBotComNovaConsola(const TCHAR* nome, const int* valor) {
	TCHAR cmdLine[256];
	PROCESS_INFORMATION pi;
	STARTUPINFO si;

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	// Monta a linha de comando
	_stprintf_s(cmdLine, 256, TEXT("bot.exe %s %d"), nome, valor);

	BOOL result = CreateProcess(
		NULL,               // Nome do programa (NULL = usa o primeiro token de cmdLine)
		cmdLine,            // Linha de comando com argumentos
		NULL,               // Segurança do processo
		NULL,               // Segurança do thread
		FALSE,              // Herda handles?
		CREATE_NEW_CONSOLE, // <-- Cria nova consola
		NULL,               // Ambiente
		NULL,               // Diretório atual
		&si,                // Startup info
		&pi                 // Process info
	);

	if (!result) {
		_tprintf(TEXT("[ERRO] ao lançar bot (%d)\n"), GetLastError());
		return FALSE;
	}
	// Fecha os handles, não precisas manter o processo aberto
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	return TRUE;
}

void tratarComando(const TCHAR* comando, LPVOID lpParam) {
	ControlData* cdata = (ControlData*)lpParam;
	DWORD bytesWritten;
	HKEY hKey;
	DWORD size = sizeof(DWORD);
	LONG result;
	TCHAR cmdPrincipal[256] = { 0 };
	TCHAR cmdSec[256] = { 0 };
	unsigned int val, n = 0, velocidade = 0;
	PipeMsg msg = { 0 };
	srand(time(NULL));
	unsigned int tempo_reacao;
	unsigned int ilider = 0;


	int args = _stscanf_s(comando, _T("%255s %255s %255s"), cmdPrincipal, 256, cmdSec, 256);

	

	if (_tcscmp(cmdPrincipal, _T("encerrar")) == 0) {
		WaitForSingleObject(cdata->hMutex, INFINITE);
		cdata->shutdown = 1;
		_tprintf(_T("A terminar...\n"));

		// Find the leader
		int ilider = 0;
		for (int i = 1; i < cdata->sharedMem->nusers; i++) {
			if (cdata->sharedMem->pontuacao[i] > cdata->sharedMem->pontuacao[ilider]) {
				ilider = i;
			}
		}

		// Prepare leader message
		PipeMsg msg;
		_tcscpy_s(msg.username, 26, _T("Arbitro"));
		msg.isUsernameInvalid = FALSE;
		_stprintf_s(msg.buff, 256, _T("Líder final: %s -> Pontuação: %d"),
			cdata->sharedMem->users[ilider],
			cdata->sharedMem->pontuacao[ilider]);

		// Send leader info to all clients
		for (int i = 0; i < cdata->sharedMem->nusers; i++) {
			if (cdata->hPipe[i] != INVALID_HANDLE_VALUE) {
				if (!WriteFile(cdata->hPipe[i], &msg, sizeof(PipeMsg), &bytesWritten, NULL)) {
					_tprintf(_T("Erro ao enviar líder para %s (%d)\n"),
						cdata->sharedMem->users[i], GetLastError());
				}
			}
		}

		// Send close command to all clients
		_tcscpy_s(msg.buff, 256, _T("close"));
		for (int i = 0; i < cdata->sharedMem->nusers; i++) {
			if (cdata->hPipe[i] != INVALID_HANDLE_VALUE) {
				if (!WriteFile(cdata->hPipe[i], &msg, sizeof(PipeMsg), &bytesWritten, NULL)) {
					_tprintf(_T("Erro ao enviar close para %s (%d)\n"),
						cdata->sharedMem->users[i], GetLastError());
				}
			}
		}
		cdata->shutdown = 1; // Sinaliza que o servidor deve terminar
		ReleaseMutex(cdata->hMutex);
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
		_tprintf(_T("Número de usuários: %d\n"), cdata->sharedMem->nusers);

		n = cdata->sharedMem->nusers;
		if (n == 0) {
			_tprintf(TEXT("Nenhum user!\n"));
			return;
		}
		_tprintf(_T("Lista de jogadores -> pontuação:\n"));
		for (int i = 0; i < cdata->sharedMem->nusers; i++) {
			_tprintf(_T("%s -> %d\n"), cdata->sharedMem->users[i], cdata->sharedMem->pontuacao[i]);
		}
	}

	else if (_tcscmp(cmdPrincipal, _T("iniciarbot")) == 0) {
		for (int i = 0; i < cdata->sharedMem->nusers; i++) {
			if (_tcscmp(cmdSec, cdata->sharedMem->users[i]) == 0) {
				_tprintf(_T("[ERRO]Nome igual a user, bot não criado\n Indicar outro nome, comando listar, para ver users\n"));
				return;
			}
		}
		velocidade = 5 + (rand() % 26);
		LancaBotComNovaConsola(cmdSec, velocidade);
	}

	else if (_tcscmp(cmdPrincipal, _T("excluir")) == 0) {
		unsigned int found = 0;
		TCHAR aEncerrar = "";
		for (n = 0; n < cdata->sharedMem->nusers; n++) {
			if (_tcscmp(cmdSec, cdata->sharedMem->users[n]) == 0) {
				found = 1;
				SetEvent(cdata->hEvSai);
				break;
			}

		}
		//enviar uma mensagem a dizer para ele fechar
		if (found == 0) {
			_tprintf(_T("Utilizador %s não encontrado\n"), cmdSec);
			return;
		}
		else {
			if (n < cdata->sharedMem->nusers) {

				_tcscpy_s(msg.username, 26, cdata->sharedMem->users[n]);
				_tcscpy_s(msg.buff, 256, _T("close")); // comando especial para o cliente sair
				msg.isUsernameInvalid = FALSE;
				WaitForSingleObject(cdata->hMutex, INFINITE);
				WriteFile(cdata->hPipe[n], &msg, sizeof(PipeMsg), &bytesWritten, NULL);
				ReleaseMutex(cdata->hMutex);

				for (n = 0; n < cdata->sharedMem->nusers; n++) {
					_tcscpy_s(msg.username, 26, cdata->sharedMem->users[n]);
					_stprintf_s(msg.buff, 256, _T("utilizador %s foi encerrado\n"), cmdSec);
					msg.isUsernameInvalid = FALSE;
					WriteFile(cdata->hPipe[0], &msg, sizeof(PipeMsg), &bytesWritten, NULL);
				}

				CloseHandle(cdata->hPipe[n]);

				// remova o utilizador do array e ajuste nusers
				for (int i = n; i < cdata->sharedMem->nusers - 1; i++) {
					WaitForSingleObject(cdata->hMutex, INFINITE);
					_tcscpy_s(cdata->sharedMem->users[i], 26, cdata->sharedMem->users[i + 1]);
					cdata->hPipe[i] = cdata->hPipe[i + 1];
					cdata->sharedMem->pontuacao[i] = cdata->sharedMem->pontuacao[i + 1];
					ReleaseMutex(cdata->hMutex);
				}
				WaitForSingleObject(cdata->hMutex, INFINITE);
				cdata->sharedMem->nusers--;
				cdata->nPipes--;
				ReleaseMutex(cdata->hMutex);
			}

			return;
		}


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
	unsigned int pontos = 0;
	unsigned int pontoslider = 0;

	const TCHAR* palavras[] = {
	   _T("carro"),
	   _T("mae"),
	   _T("pai")
	};


	// Loop principal de comunicação
	while (!cdata->shutdown) {
		// Limpa as mensagens para nova comunicação
		ZeroMemory(&receivedMsg, sizeof(PipeMsg));
		ZeroMemory(&responseMsg, sizeof(PipeMsg));

		// Lê mensagem do cliente
		fSuccess = ReadFile(hPipe, &receivedMsg, sizeof(PipeMsg), &bytesRead, NULL);


		if (!fSuccess || bytesRead != sizeof(PipeMsg)) {
			DWORD err = GetLastError();
			if (err == ERROR_BROKEN_PIPE) {
				_tprintf(_T("Cliente %s desconectou\n"), receivedMsg.username);
			}
			else {
				if (err == ERROR_INVALID_PARAMETER) {
					continue;
				}
				_tprintf(_T("Erro ao ler mensagem (%d)\n"), err);
			}
			break;
		}



		if (receivedMsg.buff[0] == _T(':')) {
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


				ReleaseMutex(cdata->hMutex);
				continue;
			}

			if (_tcscmp(receivedMsg.buff, _T(":pont")) == 0) {
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
		}
		else {
			BOOL palavraReconhecida = FALSE;
			for (int i = 0; i < sizeof(palavras) / sizeof(palavras[0]); i++) {
				if (_tcscmp(receivedMsg.buff, palavras[i]) == 0) {
					palavraReconhecida = TRUE;
					break;
				}
			}

			BOOL palavraValida = FALSE;
			size_t len = _tcslen(receivedMsg.buff);

			if (palavraReconhecida) {

				// Primeiro verifica se todas as letras existem (incluindo repetições)
				palavraValida = TRUE;

				// Cria uma cópia temporária do buffer para verificação
				TCHAR tempBuffer[BUFFER_SIZE];
				WaitForSingleObject(cdata->hMutex, INFINITE);
				for (int j = 0; j < BUFFER_SIZE; j++) {
					tempBuffer[j] = _totlower(cdata->sharedMem->buffer[j].letra);
				}
				ReleaseMutex(cdata->hMutex);

				for (size_t i = 0; i < len; i++) {
					TCHAR letra = _totlower(receivedMsg.buff[i]);
					BOOL encontrouLetra = FALSE;

					for (int j = 0; j < BUFFER_SIZE; j++) {
						if (tempBuffer[j] == letra) {
							encontrouLetra = TRUE;
							tempBuffer[j] = _T('_'); // Marca como usada
							break;
						}
					}

					if (!encontrouLetra) {
						palavraValida = FALSE;
						break;
					}
				}

				// Só remove as letras se a palavra for válida
				if (palavraValida) {
					WaitForSingleObject(cdata->hMutex, INFINITE);
					// Refaz o processo no buffer real
					for (size_t i = 0; i < len; i++) {
						TCHAR letra = _totlower(receivedMsg.buff[i]);
						for (int j = 0; j < BUFFER_SIZE; j++) {
							if (_totlower(cdata->sharedMem->buffer[j].letra) == letra) {
								cdata->sharedMem->buffer[j].letra = _T('_');
								break;
							}
						}
					}
					ReleaseMutex(cdata->hMutex);
				}
			}

			// Prepara resposta para o jogador
			if (palavraValida) {
				WaitForSingleObject(cdata->hMutex, INFINITE);
				for (int i = 0; i < cdata->sharedMem->nusers; i++) {
					if (_tcscmp(cdata->sharedMem->users[i], receivedMsg.username) == 0) {

						cdata->sharedMem->pontuacao[i] += len;
						pontos = cdata->sharedMem->pontuacao[i]; // Guarda a pontuação atualizada


						break;
					}
				}
				ReleaseMutex(cdata->hMutex);
				_stprintf_s(responseMsg.buff, 256, _T("Palavra válida! +%d pontos!\n"), len);

				// Notifica todos os outros jogadores

				_tcscpy_s(responseMsg.username, 26, _T("Arbitro"));
				responseMsg.isUsernameInvalid = FALSE;



				for (int i = 0; i < cdata->sharedMem->nusers; i++) {
					if (_tcscmp(cdata->sharedMem->users[i], receivedMsg.username) != 0 && pontos > pontoslider ) {
						_stprintf_s(responseMsg.buff, 256, _T("O jogador %s acertou a palavra '%s' e ganhou %d pontos e é o lider com %d pontos!\n"),
							receivedMsg.username, receivedMsg.buff, len, pontos);
						pontoslider = pontos;
						WriteFile(cdata->hPipe[i], &responseMsg, sizeof(PipeMsg), &bytesWritten, NULL);
					}
					else if (_tcscmp(cdata->sharedMem->users[i], receivedMsg.username) != 0) {
						_stprintf_s(responseMsg.buff, 256, _T("O jogador %s acertou a palavra '%s' e ganhou %d pontos!\n"),
							receivedMsg.username, receivedMsg.buff, len);

						WriteFile(cdata->hPipe[i], &responseMsg, sizeof(PipeMsg), &bytesWritten, NULL);
					}
				}


			}
			else {
				WaitForSingleObject(cdata->hMutex, INFINITE);
				for (int i = 0; i < cdata->sharedMem->nusers; i++) {
					if (_tcscmp(cdata->sharedMem->users[i], receivedMsg.username) == 0) {
						cdata->sharedMem->pontuacao[i] -= len / 2;
						break;
					}
				}
				ReleaseMutex(cdata->hMutex);
				_stprintf_s(responseMsg.buff, 256, _T("Palavra inválida! -%d pontos!\n"), len / 2);
			}
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

	// Notifica todos os usuários exceto o que está se desconectando
	for (int i = 0; i < cdata->sharedMem->nusers; i++) {
		// Pula o próprio usuário que está se desconectando
		if (_tcscmp(cdata->sharedMem->users[i], receivedMsg.username) == 0) {
			continue;
		}

		// Prepara mensagem de notificação
		_stprintf_s(responseMsg.buff, 256, _T("O jogador %s saiu do jogo\n"), receivedMsg.username);
		_tcscpy_s(responseMsg.username, 26, _T("Arbitro"));
		responseMsg.isUsernameInvalid = FALSE;

		// Envia a notificação
		BOOL fSuccess = WriteFile(cdata->hPipe[i], &responseMsg, sizeof(PipeMsg), &bytesWritten, NULL);

		if (!fSuccess || bytesWritten != sizeof(PipeMsg)) {
			_tprintf(_T("Erro ao enviar resposta (%d)\n"), GetLastError());
		}
	}

	// Continua a execução normalmente após notificar todos

	_tprintf(_T("Conexão com %s encerrada\n"), receivedMsg.username);
	return 0;
}


void saidaordeira(ControlData* cdata, HANDLE hTread, HANDLE mutexGlobal) {

	//função para saída ordeira
	WaitForSingleObject(cdata->hMutex, INFINITE);
	cdata->shutdown = 1; //altera a flag para que a thread termine
	ReleaseMutex(cdata->hMutex);

	//fechar os pipes
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

			break;
		}


		tratarComando(command, cdata);
	} while (cdata->shutdown == 0);

	return 0;
}

BOOL isUsernameInvalid(const TCHAR* username) {

}



int _tmain(int argc, TCHAR* argv[])
{
	LPTSTR PIPE_NAME = _T("\\\\.\\pipe\\pipeTP");
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



		if (fSuccess) {
			OVERLAPPED OverlRd = { 0 };
			HANDLE ReadReady;

			ReadReady = CreateEvent(
				NULL,   // default security attributes
				TRUE,   // manual-reset event
				FALSE,  // initial state is nonsignaled
				NULL);  // no name

			WaitForSingleObject(cdata.hMutex, INFINITE);
			cdata.hPipe[cdata.nPipes] = hPipe;
			cdata.nPipes++;
			ReleaseMutex(cdata.hMutex);

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

			fSuccess = ReadFile(hPipe, &pidata, sizeof(PipeMsg), &cbBytesRead, NULL);
			WaitForSingleObject(ReadReady, 400);
			//GetOverlappedResult(hPipe, &OverlRd, &cbBytesRead, FALSE);  // sem WAIT


			if (!fSuccess || cbBytesRead != sizeof(PipeMsg)) {
				_tprintf(_T("Erro ao ler mensagem inicial (%d)\n"), GetLastError());
				DisconnectNamedPipe(hPipe);
				CloseHandle(hPipe);
				continue;
			}

			WaitForSingleObject(cdata.hMutex, INFINITE);
			int activeUsers = 0;

			usernameExists = FALSE;
			// Verifica se username já existe
			for (int i = 0; i < cdata.sharedMem->nusers; i++) {
				if (_tcscmp(cdata.sharedMem->users[i], pidata.username) == 0) {
					usernameExists = TRUE;
					break;
				}
			}

			if (!usernameExists && cdata.sharedMem->nusers < NUSERS) {
				WaitForSingleObject(cdata.hMutex, INFINITE);
				_tcscpy_s(cdata.sharedMem->users[cdata.sharedMem->nusers], 26, pidata.username);
				cdata.sharedMem->pontuacao[cdata.sharedMem->nusers] = 0; // Define pontuação ANTES de incrementar
				cdata.sharedMem->nusers++; // Incrementa só depois
				pidata.isUsernameInvalid = FALSE;
				ReleaseMutex(cdata.hMutex);
				_tprintf(_T("Novo user registado: %s\n"), pidata.username);

				// Prepara mensagem padrão
				pidata.isUsernameInvalid = FALSE;

				for (int i = 0; i < cdata.sharedMem->nusers; i++) {
					// Pula o próprio usuário que acabou de conectar
					if (_tcscmp(cdata.sharedMem->users[i], pidata.username) == 0) {
						continue;
					}

					// Formata mensagem personalizada
					_stprintf_s(pidata.buff, 256, _T("O jogador %s juntou-se ao jogo!\n"), pidata.username);

					// Envia notificação
					BOOL success = WriteFile(
						cdata.hPipe[i],
						&pidata,
						sizeof(PipeMsg),
						&bytesWritten,
						NULL
					);

					if (!success || bytesWritten != sizeof(PipeMsg)) {
						_tprintf(_T("[AVISO] Falha ao notificar %s sobre a nova conexão (Erro: %d)\n"),
							cdata.sharedMem->users[i], GetLastError());
					}
				}


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
				_tprintf(_T("Erro Username Inválido (%d)\n"), GetLastError());
				DisconnectNamedPipe(hPipe);
				CloseHandle(hPipe);
				continue;
			}


			hMSG = CreateThread(NULL, 0, comunica, &cdata, 0, NULL);
			if (hThrTeclado == NULL) {
				_tprintf(TEXT("Error creating keyboard thread (%d)\n"), GetLastError());
				saidaordeira(&cdata, hThread, hSingle_instance);
				return 1;
			}

			if (startgame == 0 && cdata.sharedMem->nusers >= 2) {
				_tprintf(_T("Iniciando jogo com %d jogadores...\n"), cdata.sharedMem->nusers);
				startgame = 1;
				hThread = CreateThread(NULL, 0, enviaLetras, &cdata, 0, NULL);

				if (hThread == NULL) {
					_tprintf(TEXT("Error creating thread (%d)\n"), GetLastError());
					saidaordeira(&cdata, hThread, hSingle_instance);
					return 1;
				}
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


	} while (cdata.shutdown == 0);

	// Cleanup
	WaitForSingleObject(hThrTeclado, INFINITE);
	saidaordeira(&cdata, hThread, hSingle_instance);

	return 0;
}
