/* (c) Redix and Sushi */

#include <ctype.h>

#include <base/system.h>
#include <engine/serverbrowser.h>
#include <engine/shared/config.h>
#include <engine/storage.h>

#include <game/client/race.h>

#include "race_demo.h"

const char *CRaceDemo::ms_pRaceDemoDir = "demos/auto/race";

struct CDemoItem
{
	char m_aName[128];
	int m_Time;
};

struct CDemoListParam
{
	const CRaceDemo *m_pThis;
	std::vector<CDemoItem> *m_plDemos;
	const char *pMap;
};

CRaceDemo::CRaceDemo() :
	m_RaceState(RACE_NONE), m_RaceStartTick(-1), m_RecordStopTick(-1), m_Time(0) {}

void CRaceDemo::GetPath(char *pBuf, int Size, int Time) const
{
	const char *pMap = Client()->GetCurrentMap();

	char aPlayerName[MAX_NAME_LENGTH];
	str_copy(aPlayerName, Client()->PlayerName(), sizeof(aPlayerName));
	str_sanitize_filename(aPlayerName);

	if(Time < 0)
		str_format(pBuf, Size, "%s/%s_tmp_%d.demo", ms_pRaceDemoDir, pMap, pid());
	else if(g_Config.m_ClDemoName)
		str_format(pBuf, Size, "%s/%s_%d.%03d_%s.demo", ms_pRaceDemoDir, pMap, Time / 1000, Time % 1000, aPlayerName);
	else
		str_format(pBuf, Size, "%s/%s_%d.%03d.demo", ms_pRaceDemoDir, pMap, Time / 1000, Time % 1000);
}

void CRaceDemo::OnStateChange(int NewState, int OldState)
{
	if(OldState == IClient::STATE_ONLINE)
		StopRecord();
}

void CRaceDemo::OnNewSnapshot()
{
	if(!GameClient()->m_GameInfo.m_Race || !g_Config.m_ClAutoRaceRecord || Client()->State() != IClient::STATE_ONLINE)
		return;

	if(!m_pClient->m_Snap.m_pGameInfoObj || m_pClient->m_Snap.m_SpecInfo.m_Active || !m_pClient->m_Snap.m_pLocalCharacter || !m_pClient->m_Snap.m_pLocalPrevCharacter)
		return;

	static int s_LastRaceTick = -1;

	bool RaceFlag = m_pClient->m_Snap.m_pGameInfoObj->m_GameStateFlags & GAMESTATEFLAG_RACETIME;
	bool ServerControl = RaceFlag && g_Config.m_ClRaceRecordServerControl;
	int RaceTick = -m_pClient->m_Snap.m_pGameInfoObj->m_WarmupTimer;

	// start the demo
	bool ForceStart = ServerControl && s_LastRaceTick != RaceTick && Client()->GameTick(g_Config.m_ClDummy) - RaceTick < Client()->GameTickSpeed();
	bool AllowRestart = (m_AllowRestart || ForceStart) && m_RaceStartTick + 10 * Client()->GameTickSpeed() < Client()->GameTick(g_Config.m_ClDummy);
	if(m_RaceState == RACE_IDLE || m_RaceState == RACE_PREPARE || (m_RaceState == RACE_STARTED && AllowRestart))
	{
		vec2 PrevPos = vec2(m_pClient->m_Snap.m_pLocalPrevCharacter->m_X, m_pClient->m_Snap.m_pLocalPrevCharacter->m_Y);
		vec2 Pos = vec2(m_pClient->m_Snap.m_pLocalCharacter->m_X, m_pClient->m_Snap.m_pLocalCharacter->m_Y);

		if(ForceStart || (!ServerControl && CRaceHelper::IsStart(m_pClient, PrevPos, Pos)))
		{
			if(m_RaceState == RACE_STARTED)
				Client()->RaceRecord_Stop();
			if(m_RaceState != RACE_PREPARE) // start recording again
			{
				GetPath(m_aTmpFilename, sizeof(m_aTmpFilename));
				Client()->RaceRecord_Start(m_aTmpFilename);
			}
			m_RaceStartTick = Client()->GameTick(g_Config.m_ClDummy);
			m_RaceState = RACE_STARTED;
		}
	}

	// start recording before the player passes the start line, so we can see some preparation steps
	if(m_RaceState == RACE_NONE)
	{
		GetPath(m_aTmpFilename, sizeof(m_aTmpFilename));
		Client()->RaceRecord_Start(m_aTmpFilename);
		m_RaceStartTick = Client()->GameTick(g_Config.m_ClDummy);
		m_RaceState = RACE_PREPARE;
	}

	// stop recording if the player did not pass the start line after 20 seconds
	if(m_RaceState == RACE_PREPARE && Client()->GameTick(g_Config.m_ClDummy) - m_RaceStartTick >= Client()->GameTickSpeed() * 20)
	{
		StopRecord();
		m_RaceState = RACE_IDLE;
	}

	// stop the demo
	if(m_RaceState == RACE_FINISHED && m_RecordStopTick <= Client()->GameTick(g_Config.m_ClDummy))
		StopRecord(m_Time);

	s_LastRaceTick = RaceFlag ? RaceTick : -1;
}

void CRaceDemo::OnReset()
{
	StopRecord();
}

void CRaceDemo::OnMessage(int MsgType, void *pRawMsg)
{
	// check for messages from server
	if(MsgType == NETMSGTYPE_SV_KILLMSG)
	{
		CNetMsg_Sv_KillMsg *pMsg = (CNetMsg_Sv_KillMsg *)pRawMsg;
		if(pMsg->m_Victim == m_pClient->m_Snap.m_LocalClientID && Client()->RaceRecord_IsRecording())
			StopRecord(m_Time);
	}
	else if(MsgType == NETMSGTYPE_SV_CHAT)
	{
		CNetMsg_Sv_Chat *pMsg = (CNetMsg_Sv_Chat *)pRawMsg;
		if(pMsg->m_ClientID == -1 && m_RaceState == RACE_STARTED)
		{
			char aName[MAX_NAME_LENGTH];
			int Time = CRaceHelper::TimeFromFinishMessage(pMsg->m_pMessage, aName, sizeof(aName));
			if(Time > 0 && m_pClient->m_Snap.m_LocalClientID >= 0 && str_comp(aName, m_pClient->m_aClients[m_pClient->m_Snap.m_LocalClientID].m_aName) == 0)
			{
				m_RaceState = RACE_FINISHED;
				m_RecordStopTick = Client()->GameTick(g_Config.m_ClDummy) + Client()->GameTickSpeed();
				m_Time = Time;
			}
		}
	}
}

void CRaceDemo::OnMapLoad()
{
	m_AllowRestart = false;
}

void CRaceDemo::StopRecord(int Time)
{
	if(Client()->RaceRecord_IsRecording())
		Client()->RaceRecord_Stop();

	if(m_aTmpFilename[0] != '\0')
	{
		if(Time > 0 && CheckDemo(Time))
		{
			// save file
			char aNewFilename[512];
			GetPath(aNewFilename, sizeof(aNewFilename), m_Time);

			Storage()->RenameFile(m_aTmpFilename, aNewFilename, IStorage::TYPE_SAVE);
		}
		else // no new record
			Storage()->RemoveFile(m_aTmpFilename, IStorage::TYPE_SAVE);

		m_aTmpFilename[0] = '\0';
	}

	m_Time = 0;
	m_RaceState = RACE_NONE;
	m_RaceStartTick = -1;
	m_RecordStopTick = -1;
}

int CRaceDemo::RaceDemolistFetchCallback(const char *pName, time_t Date, int IsDir, int StorageType, void *pUser)
{
	CDemoListParam *pParam = (CDemoListParam *)pUser;
	int MapLen = str_length(pParam->pMap);
	if(IsDir || !str_endswith(pName, ".demo") || !str_startswith(pName, pParam->pMap) || pName[MapLen] != '_')
		return 0;

	CDemoItem Item;
	str_truncate(Item.m_aName, sizeof(Item.m_aName), pName, str_length(pName) - 5);

	const char *pTime = Item.m_aName + MapLen + 1;
	const char *pTEnd = pTime;
	while(isdigit(*pTEnd) || *pTEnd == ' ' || *pTEnd == '.' || *pTEnd == ',')
		pTEnd++;

	if(g_Config.m_ClDemoName)
	{
		char aPlayerName[MAX_NAME_LENGTH];
		str_copy(aPlayerName, pParam->m_pThis->Client()->PlayerName(), sizeof(aPlayerName));
		str_sanitize_filename(aPlayerName);

		if(pTEnd[0] != '_' || str_comp(pTEnd + 1, aPlayerName) != 0)
			return 0;
	}
	else if(pTEnd[0])
		return 0;

	Item.m_Time = CRaceHelper::TimeFromSecondsStr(pTime);
	if(Item.m_Time > 0)
		pParam->m_plDemos->push_back(Item);

	return 0;
}

bool CRaceDemo::CheckDemo(int Time) const
{
	std::vector<CDemoItem> lDemos;
	CDemoListParam Param = {this, &lDemos, Client()->GetCurrentMap()};
	Storage()->ListDirectoryInfo(IStorage::TYPE_SAVE, ms_pRaceDemoDir, RaceDemolistFetchCallback, &Param);

	// loop through demo files
	for(unsigned i = 0; i < lDemos.size(); i++)
	{
		if(Time >= lDemos[i].m_Time) // found a better demo
			return false;

		// delete old demo
		char aFilename[512];
		str_format(aFilename, sizeof(aFilename), "%s/%s.demo", ms_pRaceDemoDir, lDemos[i].m_aName);
		Storage()->RemoveFile(aFilename, IStorage::TYPE_SAVE);
	}

	return true;
}
