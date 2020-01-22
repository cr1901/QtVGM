#ifdef _WIN32
//#define _WIN32_WINNT	0x500	// for GetConsoleWindow()
#include <windows.h>
#ifdef _DEBUG
#include <crtdbg.h>
#endif
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <vector>
#include <string>

#ifdef _WIN32
extern "C" int __cdecl _getch(void);	// from conio.h
extern "C" int __cdecl _kbhit(void);
#else
#include <unistd.h>		// for STDIN_FILENO and usleep()
#include <termios.h>
#include <sys/time.h>	// for struct timeval in _kbhit()
#define	Sleep(msec)	usleep(msec * 1000)
#endif

#include "common_def.h"
#include <playthread.hpp>
#include <audio/AudioStream.h>
#include <player/vgmplayer.hpp>
#include <utils/FileLoader.h>
#include <utils/OSMutex.h>

//#define USE_MEMORY_LOADER 1	// define to use the in-memory loader
static std::string FCC2Str(UINT32 fcc);
static const char* GetFileTitle(const char* filePath);
static UINT32 CalcCurrentVolume(UINT32 playbackSmpl);
static UINT32 FillBuffer(void* drvStruct, void* userParam, UINT32 bufSize, void* Data);
static UINT8 FilePlayCallback(PlayerBase* player, void* userParam, UINT8 evtType, void* evtParam);
static UINT32 GetNthAudioDriver(UINT8 adrvType, INT32 drvNumber);
static UINT8 InitAudioSystem(void);
static UINT8 DeinitAudioSystem(void);
static UINT8 StartAudioDevice(void);
static UINT8 StopAudioDevice(void);
#ifndef _WIN32
static void changemode(UINT8 noEcho);
static int _kbhit(void);
#define	_getch	getchar
#endif

static UINT32 smplSize;
static void* audDrv;
static void* audDrvLog;
static UINT32 smplAlloc;
static WAVE_32BS* smplData;
static UINT32 localAudBufSize;
static void* localAudBuffer;
static OS_MUTEX* renderMtx;	// render thread mutex

static UINT32 sampleRate = 44100;
static UINT32 maxLoops = 2;
static bool manualRenderLoop = false;
static volatile UINT8 playState;

static UINT32 idWavOut;
static UINT32 idWavOutDev;
static UINT32 idWavWrt;

static INT32 AudioOutDrv = 0;
static INT32 WaveWrtDrv = -1;

static UINT32 masterVol = 0x10000;	// fixed point 16.16
static UINT32 fadeSmplStart;
static UINT32 fadeSmplTime;

static bool showTags = true;
static bool showFileInfo = true;

static VGMPlayer* vgmPlr;

PlayThread::PlayThread() {
  player = new VGMPlayer;
}

void PlayThread::setM3u(const char * fileName) {
  m3uFile = fileName;
}

void PlayThread::run() {
  int argc = 2;
  const char * argv[2] = {"player", m3uFile};

	int argbase;
	UINT8 retVal;
	DATA_LOADER *dLoad;
	PlayerBase* player;
	int curSong;
	bool needRefresh;

	if (argc < 2)
	{
		printf("Usage: %s inputfile\n", argv[0]);
	//	return 0;
    return;
	}
	argbase = 1;
#ifdef _WIN32
	SetConsoleOutputCP(65001);	// set UTF-8 codepage
#endif

	retVal = InitAudioSystem();
	if (retVal)
		//return 1;
    return;
	retVal = StartAudioDevice();
	if (retVal)
	{
		DeinitAudioSystem();
		//return 1;
    return;
	}
	playState = 0x00;

	// I'll keep the instances of the players for the program's life time.
	// This way player/chip options are kept between track changes.
	vgmPlr = new VGMPlayer;

	for (curSong = argbase; curSong < argc; curSong ++)
	{

	printf("Loading %s ...  ", GetFileTitle(argv[curSong]));
	fflush(stdout);
	player = NULL;

	dLoad = FileLoader_Init(argv[curSong]);
  //printf("%X, %X", curSong, dLoad->_status);

	if(dLoad == NULL) continue;

	DataLoader_SetPreloadBytes(dLoad,0x100);
	retVal = DataLoader_Load(dLoad);
	if (retVal)
	{
		DataLoader_CancelLoading(dLoad);
		fprintf(stderr, "Error 0x%02X loading file!\n", retVal);
		continue;
	}

	player = vgmPlr;
	retVal = player->LoadFile(dLoad);

	if (retVal)
	{
		DataLoader_CancelLoading(dLoad);
		player = NULL;
		fprintf(stderr, "Error 0x%02X loading file!\n", retVal);
		continue;
	}

	VGMPlayer* vgmplay = dynamic_cast<VGMPlayer*>(player);
	const VGM_HEADER* vgmhdr = vgmplay->GetFileHeader();

	printf("VGM v%3X, Total Length: %.2f s, Loop Length: %.2f s", vgmhdr->fileVer,
			player->Tick2Second(player->GetTotalTicks()), player->Tick2Second(player->GetLoopTicks()));

	const char* songTitle = NULL;
	const char* songAuthor = NULL;
	const char* songGame = NULL;
	const char* songSystem = NULL;
	const char* songDate = NULL;
	const char* songComment = NULL;

  const char* const* tagList = player->GetTags();
	for (const char* const* t = tagList; *t; t += 2)
	{
		if (!strcmp(t[0], "TITLE"))
			songTitle = t[1];
		else if (!strcmp(t[0], "ARTIST"))
			songAuthor = t[1];
		else if (!strcmp(t[0], "GAME"))
			songGame = t[1];
		else if (!strcmp(t[0], "SYSTEM"))
			songSystem = t[1];
		else if (!strcmp(t[0], "DATE"))
			songDate = t[1];
		else if (!strcmp(t[0], "COMMENT"))
			songComment = t[1];
	}

  UINT16 offset = 0;

  // FIXME: Check that offset doesn't overflow.
	if (songTitle != NULL && songTitle[0] != '\0')
		offset += snprintf(&infoBuf[offset], 1024 - offset, "Title: %s", songTitle);
	if (songAuthor != NULL && songAuthor[0] != '\0')
    offset += snprintf(&infoBuf[offset], 1024 - offset, "\nAuthor: %s", songAuthor);
	if (songGame != NULL && songGame[0] != '\0')
    offset += snprintf(&infoBuf[offset], 1024 - offset, "\nGame: %s", songGame);
	if (songSystem != NULL && songSystem[0] != '\0')
    offset += snprintf(&infoBuf[offset], 1024 - offset, "\nSystem: %s", songSystem);
	if (songDate != NULL && songDate[0] != '\0')
    offset += snprintf(&infoBuf[offset], 1024 - offset, "\nDate: %s", songDate);
	// if (songComment != NULL && songComment[0] != '\0')
  //   offset += snprintf(&infoBuf[offset], 1024 - offset, "\nComment: %s", songComment);

  emit newSong(infoBuf);

	putchar('\n');

	player->SetSampleRate(sampleRate);
	player->Start();
	fadeSmplTime = player->GetSampleRate() * 4;

  // TODO: Make this not hardcoded.
	fadeSmplStart = player->Tick2Sample(player->GetTotalPlayTicks(maxLoops));

	if (showFileInfo)
	{
		PLR_SONG_INFO sInf;
		std::vector<PLR_DEV_INFO> diList;
		size_t curDev;

		player->GetSongInfo(sInf);
		player->GetSongDeviceInfo(diList);
		printf("SongInfo: %s v%X.%X, Rate %u/%u, Len %u, Loop at %d, devices: %u\n",
			FCC2Str(sInf.format).c_str(), sInf.fileVerMaj, sInf.fileVerMin,
			sInf.tickRateMul, sInf.tickRateDiv, sInf.songLen, sInf.loopTick, sInf.deviceCnt);
		for (curDev = 0; curDev < diList.size(); curDev ++)
		{
			const PLR_DEV_INFO& pdi = diList[curDev];
			printf(" Dev %d: Type 0x%02X #%d, Core %s, Clock %u, Rate %u, Volume 0x%X\n",
				(int)pdi.id, pdi.type, (INT8)pdi.instance, FCC2Str(pdi.core).c_str(), pdi.clock, pdi.smplRate, pdi.volume);
			if (pdi.cParams != 0)
				printf("        CfgParams: 0x%08X\n", pdi.cParams);
		}
	}

	if (audDrv != NULL)
		retVal = AudioDrv_SetCallback(audDrv, FillBuffer, &player);
	else
		retVal = 0xFF;
	manualRenderLoop = (retVal != 0x00);
#ifndef _WIN32
	changemode(1);
#endif
	playState &= ~PLAYSTATE_END;
	needRefresh = true;
	while(! (playState & PLAYSTATE_END))
	{
		if (! (playState & PLAYSTATE_PAUSE))
			needRefresh = true;	// always update when playing
		if (needRefresh)
		{
			const char* pState;

			if (playState & PLAYSTATE_PAUSE)
				pState = "Paused";
			else
				pState = "Playing";
			printf("%s %.2f / %.2f ...   \r", pState, player->Sample2Second(player->GetCurPos(PLAYPOS_SAMPLE)),
					player->Tick2Second(player->GetTotalPlayTicks(maxLoops)));
			fflush(stdout);
			needRefresh = false;
		}

		if (manualRenderLoop && ! (playState & PLAYSTATE_PAUSE))
		{
			UINT32 wrtBytes = FillBuffer(audDrvLog, &player, localAudBufSize, localAudBuffer);
			AudioDrv_WriteData(audDrvLog, wrtBytes, localAudBuffer);
		}
		else
		{
			Sleep(50);
		}

		if (_kbhit())
		{
			int inkey = _getch();
			int letter = toupper(inkey);

			if (letter == ' ' || letter == 'P')
			{
				playState ^= PLAYSTATE_PAUSE;
				if (audDrv != NULL)
				{
					if (playState & PLAYSTATE_PAUSE)
						AudioDrv_Pause(audDrv);
					else
						AudioDrv_Resume(audDrv);
				}
			}
			else if (letter == 'B')	// previous file
			{
				if (curSong > argbase)
				{
					playState |= PLAYSTATE_END;
					curSong -= 2;
				}
			}
			else if (letter == 'N')	// next file
			{
				if (curSong + 1 < argc)
					playState |= PLAYSTATE_END;
			}
			else if (inkey == 0x1B || letter == 'Q')	// quit
			{
				playState |= PLAYSTATE_END;
				curSong = argc - 1;
			}
			needRefresh = true;
		}
	}
#ifndef _WIN32
	changemode(0);
#endif
	// remove callback to prevent further rendering
	// also waits for render thread to finish its work
	if (audDrv != NULL)
		AudioDrv_SetCallback(audDrv, NULL, NULL);

	player->Stop();
	player->UnloadFile();
	DataLoader_Deinit(dLoad);
	player = NULL; dLoad = NULL;
	}	// end for(curSong)

	delete vgmPlr;

	StopAudioDevice();
	DeinitAudioSystem();
	printf("Done.\n");

#if defined(_DEBUG) && (_MSC_VER >= 1400)
	// doesn't work well with C++ containers
	//if (_CrtDumpMemoryLeaks())
	//	_getch();
#endif

	//return 0;
  return;
}

static std::string FCC2Str(UINT32 fcc)
{
	std::string result(4, '\0');
	result[0] = (char)((fcc >> 24) & 0xFF);
	result[1] = (char)((fcc >> 16) & 0xFF);
	result[2] = (char)((fcc >>  8) & 0xFF);
	result[3] = (char)((fcc >>  0) & 0xFF);
	return result;
}

static const char* GetFileTitle(const char* filePath)
{
	const char* dirSep1;
	const char* dirSep2;

	dirSep1 = strrchr(filePath, '/');
	dirSep2 = strrchr(filePath, '\\');
	if (dirSep2 > dirSep1)
		dirSep1 = dirSep2;

	return (dirSep1 == NULL) ? filePath : (dirSep1 + 1);
}

#if 1
#define VOLCALC64
#define VOL_BITS	16	// use .X fixed point for working volume
#else
#define VOL_BITS	8	// use .X fixed point for working volume
#endif
#define VOL_SHIFT	(16 - VOL_BITS)	// shift for master volume -> working volume

// Pre- and post-shifts are used to make the calculations as accurate as possible
// without causing the sample data (likely 24 bits) to overflow while applying the volume gain.
// Smaller values for VOL_PRESH are more accurate, but have a higher risk of overflows during calculations.
// (24 + VOL_POSTSH) must NOT be larger than 31
#define VOL_PRESH	4	// sample data pre-shift
#define VOL_POSTSH	(VOL_BITS - VOL_PRESH)	// post-shift after volume multiplication

static UINT32 CalcCurrentVolume(UINT32 playbackSmpl)
{
	UINT32 curVol;	// 16.16 fixed point

	// 1. master volume
	curVol = masterVol;

	// 2. apply fade-out factor
	if (playbackSmpl >= fadeSmplStart)
	{
		UINT32 fadeSmpls;
		UINT64 fadeVol;	// 64 bit for less type casts when doing multiplications with .16 fixed point

		fadeSmpls = playbackSmpl - fadeSmplStart;
		if (fadeSmpls >= fadeSmplTime)
			return 0x0000;	// going beyond fade time -> volume 0

		fadeVol = (UINT64)fadeSmpls * 0x10000 / fadeSmplTime;
		fadeVol = 0x10000 - fadeVol;	// fade from full volume to silence
		fadeVol = fadeVol * fadeVol;	// logarithmic fading sounds nicer
		curVol = (UINT32)((fadeVol * curVol) >> 32);
	}

	return curVol;
}

static UINT32 FillBuffer(void* drvStruct, void* userParam, UINT32 bufSize, void* data)
{
	PlayerBase* player;
	UINT32 basePbSmpl;
	UINT32 smplCount;
	UINT32 smplRendered;
	INT16* SmplPtr16;
	UINT32 curSmpl;
	WAVE_32BS fnlSmpl;	// final sample value
	INT32 curVolume;

	smplCount = bufSize / smplSize;
	if (! smplCount)
		return 0;

	player = *(PlayerBase**)userParam;
	if (player == NULL)
	{
		memset(data, 0x00, smplCount * smplSize);
		return smplCount * smplSize;
	}
	if (! (player->GetState() & PLAYSTATE_PLAY))
	{
		fprintf(stderr, "Player Warning: calling Render while not playing! playState = 0x%02X\n", player->GetState());
		memset(data, 0x00, smplCount * smplSize);
		return smplCount * smplSize;
	}

	OSMutex_Lock(renderMtx);
	if (smplCount > smplAlloc)
		smplCount = smplAlloc;
	memset(smplData, 0, smplCount * sizeof(WAVE_32BS));
	basePbSmpl = player->GetCurPos(PLAYPOS_SAMPLE);
	smplRendered = player->Render(smplCount, smplData);
	smplCount = smplRendered;

	curVolume = (INT32)CalcCurrentVolume(basePbSmpl) >> VOL_SHIFT;
	SmplPtr16 = (INT16*)data;
	for (curSmpl = 0; curSmpl < smplCount; curSmpl ++, basePbSmpl ++, SmplPtr16 += 2)
	{
		if (basePbSmpl >= fadeSmplStart)
		{
			UINT32 fadeSmpls;

			fadeSmpls = basePbSmpl - fadeSmplStart;
			if (fadeSmpls >= fadeSmplTime && ! (playState & PLAYSTATE_END))
			{
				playState |= PLAYSTATE_END;
				break;
			}

			curVolume = (INT32)CalcCurrentVolume(basePbSmpl) >> VOL_SHIFT;
		}

		// Input is about 24 bits (some cores might output a bit more)
		fnlSmpl = smplData[curSmpl];

#ifdef VOLCALC64
		fnlSmpl.L = (INT32)( ((INT64)fnlSmpl.L * curVolume) >> VOL_BITS );
		fnlSmpl.R = (INT32)( ((INT64)fnlSmpl.R * curVolume) >> VOL_BITS );
#else
		fnlSmpl.L = ((fnlSmpl.L >> VOL_PRESH) * curVolume) >> VOL_POSTSH;
		fnlSmpl.R = ((fnlSmpl.R >> VOL_PRESH) * curVolume) >> VOL_POSTSH;
#endif

		fnlSmpl.L >>= 8;	// 24 bit -> 16 bit
		fnlSmpl.R >>= 8;
		if (fnlSmpl.L < -0x8000)
			fnlSmpl.L = -0x8000;
		else if (fnlSmpl.L > +0x7FFF)
			fnlSmpl.L = +0x7FFF;
		if (fnlSmpl.R < -0x8000)
			fnlSmpl.R = -0x8000;
		else if (fnlSmpl.R > +0x7FFF)
			fnlSmpl.R = +0x7FFF;
		SmplPtr16[0] = (INT16)fnlSmpl.L;
		SmplPtr16[1] = (INT16)fnlSmpl.R;
	}
	OSMutex_Unlock(renderMtx);

	return curSmpl * smplSize;
}

static UINT32 GetNthAudioDriver(UINT8 adrvType, INT32 drvNumber)
{
	if (drvNumber == -1)
		return (UINT32)-1;

	UINT32 drvCount;
	UINT32 curDrv;
	INT32 typedDrv;
	AUDDRV_INFO* drvInfo;

	// go through all audio drivers get the ID of the requested Output/Disk Writer driver
	drvCount = Audio_GetDriverCount();
	for (typedDrv = 0, curDrv = 0; curDrv < drvCount; curDrv ++)
	{
		Audio_GetDriverInfo(curDrv, &drvInfo);
		if (drvInfo->drvType == adrvType)
		{
			if (typedDrv == drvNumber)
				return curDrv;
			typedDrv ++;
		}
	}

	return (UINT32)-1;
}

// initialize audio system and search for requested audio drivers
static UINT8 InitAudioSystem(void)
{
	AUDDRV_INFO* drvInfo;
	UINT8 retVal;

	retVal = OSMutex_Init(&renderMtx, 0);

	printf("Opening Audio Device ...\n");
	retVal = Audio_Init();
	if (retVal == AERR_NODRVS)
		return retVal;

	idWavOut = GetNthAudioDriver(ADRVTYPE_OUT, AudioOutDrv);
	idWavOutDev = 0;	// default device
	if (AudioOutDrv != -1 && idWavOut == (UINT32)-1)
	{
		fprintf(stderr, "Requested Audio Output driver not found!\n");
		Audio_Deinit();
		return AERR_NODRVS;
	}
	idWavWrt = GetNthAudioDriver(ADRVTYPE_DISK, WaveWrtDrv);

	audDrv = NULL;
	if (idWavOut != (UINT32)-1)
	{
		Audio_GetDriverInfo(idWavOut, &drvInfo);
		printf("Using driver %s.\n", drvInfo->drvName);
		retVal = AudioDrv_Init(idWavOut, &audDrv);
		if (retVal)
		{
			fprintf(stderr, "WaveOut: Driver Init Error: %02X\n", retVal);
			Audio_Deinit();
			return retVal;
		}
#ifdef AUDDRV_DSOUND
		if (drvInfo->drvSig == ADRVSIG_DSOUND)
			DSound_SetHWnd(AudioDrv_GetDrvData(audDrv), GetDesktopWindow());
#endif
	}

	audDrvLog = NULL;
	if (idWavWrt != (UINT32)-1)
	{
		retVal = AudioDrv_Init(idWavWrt, &audDrvLog);
		if (retVal)
			audDrvLog = NULL;
	}

	return 0x00;
}

static UINT8 DeinitAudioSystem(void)
{
	UINT8 retVal;

	retVal = 0x00;
	if (audDrv != NULL)
	{
		retVal = AudioDrv_Deinit(&audDrv);	audDrv = NULL;
	}
	if (audDrvLog != NULL)
	{
		AudioDrv_Deinit(&audDrvLog);	audDrvLog = NULL;
	}
	Audio_Deinit();

	OSMutex_Deinit(renderMtx);	renderMtx = NULL;

	return retVal;
}

static UINT8 StartAudioDevice(void)
{
	AUDIO_OPTS* opts;
	UINT8 retVal;

	opts = NULL;
	smplAlloc = 0x00;
	smplData = NULL;

	if (audDrv != NULL)
		opts = AudioDrv_GetOptions(audDrv);
	else if (audDrvLog != NULL)
		opts = AudioDrv_GetOptions(audDrvLog);
	if (opts == NULL)
		return 0xFF;
	opts->sampleRate = sampleRate;
	opts->numChannels = 2;
	opts->numBitsPerSmpl = 16;
	smplSize = opts->numChannels * opts->numBitsPerSmpl / 8;

	if (audDrv != NULL)
	{
		printf("Opening Device %u ...\n", idWavOutDev);
		retVal = AudioDrv_Start(audDrv, idWavOutDev);
		if (retVal)
		{
			fprintf(stderr, "Device Init Error: %02X\n", retVal);
			return retVal;
		}

		smplAlloc = AudioDrv_GetBufferSize(audDrv) / smplSize;
		localAudBufSize = 0;
	}
	else
	{
		smplAlloc = opts->sampleRate / 4;
		localAudBufSize = smplAlloc * smplSize;
	}

	smplData = (WAVE_32BS*)malloc(smplAlloc * sizeof(WAVE_32BS));
	localAudBuffer = localAudBufSize ? malloc(localAudBufSize) : NULL;

	return 0x00;
}

static UINT8 StopAudioDevice(void)
{
	UINT8 retVal;

	retVal = 0x00;
	if (audDrv != NULL)
		retVal = AudioDrv_Stop(audDrv);
	free(smplData);	smplData = NULL;
	free(localAudBuffer);	localAudBuffer = NULL;

	return retVal;
}

#ifndef _WIN32
static struct termios oldterm;
static UINT8 termmode = 0xFF;

static void changemode(UINT8 noEcho)
{
	if (termmode == 0xFF)
	{
		tcgetattr(STDIN_FILENO, &oldterm);
		termmode = 0;
	}
	if (termmode == noEcho)
		return;

	if (noEcho)
	{
		struct termios newterm;
		newterm = oldterm;
		newterm.c_lflag &= ~(ICANON | ECHO);
		tcsetattr(STDIN_FILENO, TCSANOW, &newterm);
		termmode = 1;
	}
	else
	{
		tcsetattr(STDIN_FILENO, TCSANOW, &oldterm);
		termmode = 0;
	}

	return;
}

static int _kbhit(void)
{
	struct timeval tv;
	fd_set rdfs;

	tv.tv_sec = 0;
	tv.tv_usec = 0;

	FD_ZERO(&rdfs);
	FD_SET(STDIN_FILENO, &rdfs);
	select(STDIN_FILENO + 1, &rdfs, NULL, NULL, &tv);

	return FD_ISSET(STDIN_FILENO, &rdfs);;
}
#endif
