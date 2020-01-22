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

#ifndef _WIN32
#include <unistd.h>		// for STDIN_FILENO and usleep()
#include <termios.h>
#define	Sleep(msec)	usleep(msec * 1000)
#endif

#ifdef WIN32
#define DIR_CHR		'\\'
#else
#define DIR_CHR		'/'
#endif

#include "common_def.h"
#include <playthread.hpp>
#include <audio/AudioStream.h>
#include <player/vgmplayer.hpp>
#include <utils/FileLoader.h>
#include <utils/OSMutex.h>

static UINT32 CalcCurrentVolume(UINT32 playbackSmpl);
static UINT32 FillBuffer(void* drvStruct, void* userParam, UINT32 bufSize, void* Data);
static UINT32 GetNthAudioDriver(UINT8 adrvType, INT32 drvNumber);
static UINT8 InitAudioSystem(void);
static UINT8 DeinitAudioSystem(void);
static UINT8 StartAudioDevice(void);
static UINT8 StopAudioDevice(void);
static bool OpenPlayListFile(const char* FileName, char*** PlayListFile);
static void StandardizeDirSeparators(char* FilePath);
char* GetLastDirSeparator(const char* FilePath);
static bool IsAbsolutePath(const char* FilePath);
#ifndef _WIN32
static void changemode(UINT8 noEcho);
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

static VGMPlayer* vgmPlr;

static char PLFileBase[MAX_PATH];
UINT32 PLFileCount;
UINT32 CurPLFile;

PlayThread::PlayThread() {
  player = new VGMPlayer;
}

void PlayThread::postKeyCode(int key) {
  keyMutex.lock();
  keyCode = key;
  keyPressed = true;
  keyMutex.unlock();
}

bool PlayThread::kbPress(int * key) {
  if(!keyMutex.tryLock())
  {
    return false;
  }

  if(keyPressed)
  {
    (* key) = keyCode;
    keyPressed = false;
    keyMutex.unlock();
    return true;
  }

  keyMutex.unlock();
  return false;
}

void PlayThread::setM3u(const char * fileName) {
  m3uFile = fileName;
}

void PlayThread::run() {
	UINT8 retVal;
	DATA_LOADER *dLoad;
	PlayerBase* player;
	int curSong;
	bool needRefresh;

#ifdef _WIN32
	SetConsoleOutputCP(65001);	// set UTF-8 codepage
#endif

	retVal = InitAudioSystem();
	if (retVal)
    return;
	retVal = StartAudioDevice();
	if (retVal)
	{
		DeinitAudioSystem();
    return;
	}
	playState = 0x00;

	// I'll keep the instances of the players for the program's life time.
	// This way player/chip options are kept between track changes.
	vgmPlr = new VGMPlayer;
  char ** PlayListFileContents;

  if(!OpenPlayListFile(m3uFile, &PlayListFileContents))
  {
    return;
  }

  for (CurPLFile = 0x00; CurPLFile < PLFileCount; CurPLFile ++)
  {
    char vgmFile[MAX_PATH];

  	fflush(stdout);
  	player = NULL;

    if (IsAbsolutePath(PlayListFileContents[CurPLFile]))
    {
      strcpy(vgmFile, PlayListFileContents[CurPLFile]);
    }
    else
    {
      strcpy(vgmFile, PLFileBase);
      strcat(vgmFile, PlayListFileContents[CurPLFile]);
    }

  	dLoad = FileLoader_Init(vgmFile);
  	if(dLoad == NULL) continue;

  	DataLoader_SetPreloadBytes(dLoad,0x100);
  	retVal = DataLoader_Load(dLoad);
  	if (retVal)
  	{
  		DataLoader_CancelLoading(dLoad);
  		continue;
  	}

  	player = vgmPlr;
  	retVal = player->LoadFile(dLoad);

  	if (retVal)
  	{
  		DataLoader_CancelLoading(dLoad);
  		player = NULL;
  		continue;
  	}

  	VGMPlayer* vgmplay = dynamic_cast<VGMPlayer*>(player);
  	const VGM_HEADER* vgmhdr = vgmplay->GetFileHeader();

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

  	player->SetSampleRate(sampleRate);
  	player->Start();
  	fadeSmplTime = player->GetSampleRate() * 4;

    // TODO: Make this not hardcoded.
  	fadeSmplStart = player->Tick2Sample(player->GetTotalPlayTicks(maxLoops));

  	// if (showFileInfo)
  	// {
  	// 	PLR_SONG_INFO sInf;
  	// 	std::vector<PLR_DEV_INFO> diList;
  	// 	size_t curDev;
    //
  	// 	player->GetSongInfo(sInf);
  	// 	player->GetSongDeviceInfo(diList);
  	// 	printf("SongInfo: %s v%X.%X, Rate %u/%u, Len %u, Loop at %d, devices: %u\n",
  	// 		FCC2Str(sInf.format).c_str(), sInf.fileVerMaj, sInf.fileVerMin,
  	// 		sInf.tickRateMul, sInf.tickRateDiv, sInf.songLen, sInf.loopTick, sInf.deviceCnt);
  	// 	for (curDev = 0; curDev < diList.size(); curDev ++)
  	// 	{
  	// 		const PLR_DEV_INFO& pdi = diList[curDev];
  	// 		printf(" Dev %d: Type 0x%02X #%d, Core %s, Clock %u, Rate %u, Volume 0x%X\n",
  	// 			(int)pdi.id, pdi.type, (INT8)pdi.instance, FCC2Str(pdi.core).c_str(), pdi.clock, pdi.smplRate, pdi.volume);
  	// 		if (pdi.cParams != 0)
  	// 			printf("        CfgParams: 0x%08X\n", pdi.cParams);
  	// 	}
  	// }

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
  		if (manualRenderLoop && ! (playState & PLAYSTATE_PAUSE))
  		{
  			UINT32 wrtBytes = FillBuffer(audDrvLog, &player, localAudBufSize, localAudBuffer);
  			AudioDrv_WriteData(audDrvLog, wrtBytes, localAudBuffer);
  		}
  		else
  		{
  			Sleep(50);
  		}

      int inkey;
  		if (kbPress(&inkey))
  		{
  			if (inkey == Qt::Key_Space || inkey == Qt::Key_P)
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
  			else if (inkey == Qt::Key_B)	// previous file
  			{
  				if (CurPLFile > 0)
  				{
  					playState |= PLAYSTATE_END;
  					CurPLFile -= 2;
  				}
  			}
  			else if (inkey == Qt::Key_N)	// next file
  			{
  				if (CurPLFile + 1 < PLFileCount)
  					playState |= PLAYSTATE_END;
  			}
  			else if ( /* inkey == 0x1B || */ inkey == Qt::Key_Q)	// quit
  			{
  				playState |= PLAYSTATE_END;
  				CurPLFile = PLFileCount - 1;
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

#if defined(_DEBUG) && (_MSC_VER >= 1400)
	// doesn't work well with C++ containers
	//if (_CrtDumpMemoryLeaks())
	//	_getch();
#endif

  return;
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

	retVal = Audio_Init();
	if (retVal == AERR_NODRVS)
		return retVal;

	idWavOut = GetNthAudioDriver(ADRVTYPE_OUT, AudioOutDrv);
	idWavOutDev = 0;	// default device
	if (AudioOutDrv != -1 && idWavOut == (UINT32)-1)
	{
		Audio_Deinit();
		return AERR_NODRVS;
	}
	idWavWrt = GetNthAudioDriver(ADRVTYPE_DISK, WaveWrtDrv);

	audDrv = NULL;
	if (idWavOut != (UINT32)-1)
	{
		Audio_GetDriverInfo(idWavOut, &drvInfo);
		retVal = AudioDrv_Init(idWavOut, &audDrv);
		if (retVal)
		{
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
		retVal = AudioDrv_Start(audDrv, idWavOutDev);
		if (retVal)
		{
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
#endif

static bool OpenPlayListFile(const char* FileName, char*** PlayListFile)
{
	const char M3UV2_HEAD[] = "#EXTM3U";
	const char M3UV2_META[] = "#EXTINF:";
	const UINT8 UTF8_SIG[] = {0xEF, 0xBB, 0xBF};
	UINT32 METASTR_LEN;
	size_t RetVal;

	FILE* hFile;
	UINT32 LineNo;
	bool IsV2Fmt;
	UINT32 PLAlloc;
	char TempStr[0x1000];	// 4096 chars should be enough
	char* RetStr;
	bool IsUTF8;

	hFile = fopen(FileName, "rt");
	if (hFile == NULL)
		return false;

	RetVal = fread(TempStr, 0x01, 0x03, hFile);
	if (RetVal >= 0x03)
		IsUTF8 = ! memcmp(TempStr, UTF8_SIG, 0x03);
	else
		IsUTF8 = false;

	rewind(hFile);

	PLAlloc = 0x0100;
	PLFileCount = 0x00;
	LineNo = 0x00;
	IsV2Fmt = false;
	METASTR_LEN = strlen(M3UV2_META);
	(* PlayListFile) = (char**)malloc(PLAlloc * sizeof(char*));
	while(! feof(hFile))
	{
		RetStr = fgets(TempStr, 0x1000, hFile);
		if (RetStr == NULL)
			break;
		//RetStr = strchr(TempStr, 0x0D);
		//if (RetStr)
		//	*RetStr = 0x00;	// remove NewLine-Character
		RetStr = TempStr + strlen(TempStr) - 0x01;
		while(RetStr >= TempStr && *RetStr < 0x20)
		{
			*RetStr = '\0';	// remove NewLine-Characters
			RetStr --;
		}
		if (! strlen(TempStr))
			continue;

		if (! LineNo)
		{
			if (! strcmp(TempStr, M3UV2_HEAD))
			{
				IsV2Fmt = true;
				LineNo ++;
				continue;
			}
		}
		if (IsV2Fmt)
		{
			if (! strncmp(TempStr, M3UV2_META, METASTR_LEN))
			{
				// Ignore Metadata of m3u Version 2
				LineNo ++;
				continue;
			}
		}

		if (PLFileCount >= PLAlloc)
		{
			PLAlloc += 0x0100;
			(* PlayListFile) = (char**)realloc((* PlayListFile), PLAlloc * sizeof(char*));
		}

		// TODO:
		//	- supprt UTF-8 m3us under Windows
		//	- force IsUTF8 via Commandline
#ifdef WIN32
		// Windows uses the 1252 Codepage by default
		(* PlayListFile)[PLFileCount] = (char*)malloc((strlen(TempStr) + 0x01) * sizeof(char));
		strcpy((* PlayListFile)[PLFileCount], TempStr);
#else
		if (! IsUTF8)
		{
			// Most recent Linux versions use UTF-8, so I need to convert all strings.
			ConvertCP1252toUTF8(PlayListFile[PLFileCount], TempStr);
		}
		else
		{
			(* PlayListFile)[PLFileCount] = (char*)malloc((strlen(TempStr) + 0x01) * sizeof(char));
			strcpy((* PlayListFile)[PLFileCount], TempStr);
		}
#endif
		StandardizeDirSeparators((* PlayListFile)[PLFileCount]);
		PLFileCount ++;
		LineNo ++;
	}

	fclose(hFile);

	RetStr = GetLastDirSeparator(FileName);
	if (RetStr != NULL)
	{
		RetStr ++;
		strncpy(PLFileBase, FileName, RetStr - FileName);
		PLFileBase[RetStr - FileName] = '\0';
		StandardizeDirSeparators(PLFileBase);
	}
	else
	{
		strcpy(PLFileBase, "");
	}

	return true;
}

static void StandardizeDirSeparators(char* FilePath)
{
	char* CurChr;

	CurChr = FilePath;
	while(*CurChr != '\0')
	{
		if (*CurChr == '\\' || *CurChr == '/')
			*CurChr = DIR_CHR;
		CurChr ++;
	}

	return;
}

char* GetLastDirSeparator(const char* FilePath)
{
	char* SepPos1;
	char* SepPos2;

	SepPos1 = strrchr(FilePath, '/');
	SepPos2 = strrchr(FilePath, '\\');
	if (SepPos1 < SepPos2)
		return SepPos2;
	else
		return SepPos1;
}

static bool IsAbsolutePath(const char* FilePath)
{
#ifdef WIN32
	if (FilePath[0] == '\0')
		return false;	// empty string
	if (FilePath[1] == ':')
		return true;	// Device Path: C:\path
	if (! strncmp(FilePath, "\\\\", 2))
		return true;	// Network Path: \\computername\path
#else
	if (FilePath[0] == '/')
		return true;	// absolute UNIX path
#endif
	return false;
}
