#include "global.h"
#include "NetworkSyncManager.h"
#include "LuaManager.h"
#include "LocalizedString.h"
#include <errno.h>

NetworkSyncManager *NSMAN;

#if defined(WITHOUT_NETWORKING)
NetworkSyncManager::NetworkSyncManager( LoadingWindow *ld ) { useSMserver=false; isSMOnline = false; }
NetworkSyncManager::~NetworkSyncManager () { }
void NetworkSyncManager::CloseConnection() { }
void NetworkSyncManager::PostStartUp( const RString& ServerIP ) { }
bool NetworkSyncManager::Connect( const RString& addy, unsigned short port ) { return false; }
RString NetworkSyncManager::GetServerName() { return RString(); }
void NetworkSyncManager::ReportNSSOnOff( int i ) { }
void NetworkSyncManager::ReportScore( int playerID, int step, int score, int combo, float offset ) { }
void NetworkSyncManager::ReportSongOver() { }
void NetworkSyncManager::ReportStyle() {}
void NetworkSyncManager::StartRequest( short position ) { }
void NetworkSyncManager::DisplayStartupStatus() { }
void NetworkSyncManager::Update( float fDeltaTime ) { }
bool NetworkSyncManager::ChangedScoreboard( int Column ) { return false; }
void NetworkSyncManager::SendChat( const RString& message ) { }
void NetworkSyncManager::SelectUserSong() { }
RString NetworkSyncManager::MD5Hex( const RString &sInput ) { return RString(); }
int NetworkSyncManager::GetSMOnlineSalt() { return 0; }
void NetworkSyncManager::GetListOfLANServers( vector<NetServerInfo>& AllServers ) { } 
#else
#include "ezsockets.h"
#include "NetworkPacket.h"
#include "ProfileManager.h"
#include "RageLog.h"
#include "ScreenManager.h"
#include "Song.h"
#include "Course.h"
#include "GameState.h"
#include "StatsManager.h"
#include "Steps.h"
#include "ProductInfo.h"
#include "ScreenMessage.h"
#include "GameManager.h"
#include "MessageManager.h"
#include "arch/LoadingWindow/LoadingWindow.h"
#include "PlayerState.h"
#include "CryptManager.h"

AutoScreenMessage( SM_AddToChat );
AutoScreenMessage( SM_ChangeSong );
AutoScreenMessage( SM_GotEval );
AutoScreenMessage( SM_UsersUpdate );
AutoScreenMessage( SM_SMOnlinePack );

int NetworkSyncManager::GetSMOnlineSalt()
{
	return m_iSalt;
}

static LocalizedString INITIALIZING_CLIENT_NETWORK	( "NetworkSyncManager", "Initializing Client Network..." );
NetworkSyncManager::NetworkSyncManager( LoadingWindow *ld )
{
	LANserver = NULL;	//So we know if it has been created yet
	BroadcastReception = NULL;

	ld->SetText( INITIALIZING_CLIENT_NETWORK );
	NetPlayerClient = new EzSockets;
	NetPlayerClient->blocking = false;
	m_ServerVersion = 0;
   
	useSMserver = false;
	isSMOnline = false;
	FOREACH_PlayerNumber( pn )
		isSMOLoggedIn[pn] = false;

	m_startupStatus = 0;	//By default, connection not tried.

	m_ActivePlayers = 0;

	StartUp();
}

NetworkSyncManager::~NetworkSyncManager ()
{
	//Close Connection to server nicely.
	if( useSMserver )
		NetPlayerClient->close();
	SAFE_DELETE( NetPlayerClient );

	if ( BroadcastReception ) 
	{
		BroadcastReception->close();
		SAFE_DELETE( BroadcastReception );
	}
}

void NetworkSyncManager::CloseConnection()
{
	if( !useSMserver )
		return;
	m_ServerVersion = 0;
   	useSMserver = false;
	isSMOnline = false;
	FOREACH_PlayerNumber( pn )
		isSMOLoggedIn[pn] = false;
	m_startupStatus = 0;
	NetPlayerClient->close();
}

void NetworkSyncManager::PostStartUp( const RString& ServerIP )
{
	RString sAddress;
	unsigned short iPort;

	size_t cLoc = ServerIP.find( ':' );
	if( ServerIP.find( ':' ) != RString::npos )
	{
		sAddress = ServerIP.substr( 0, cLoc );
		char* cEnd;
		errno = 0;
		iPort = (unsigned short)strtol( ServerIP.substr( cLoc + 1 ).c_str(), &cEnd, 10 );
		if( *cEnd != 0 || errno != 0 )
		{
			m_startupStatus = 2;
			LOG->Warn( "Invalid port" );
			return;
		}
	}
	else
	{
		iPort = 8765;
		sAddress = ServerIP;
	}

	LOG->Info( "Attempting to connect to: %s, Port: %i", sAddress.c_str(), iPort );

	CloseConnection();
	if( !Connect(sAddress.c_str(), iPort) )
	{
		m_startupStatus = 2;
		LOG->Warn( "Network Sync Manager failed to connect" );
		return;
	}

	FOREACH_PlayerNumber( pn )
		isSMOLoggedIn[pn] = false;

	useSMserver = true;

	m_startupStatus = 1;	// Connection attepmpt successful

	// If network play is desired and the connection works,
	// halt until we know what server version we're dealing with

	m_packet.Clear();
	m_packet.Write1( NSCHello );	//Hello Packet

	m_packet.Write1( NETPROTOCOLVERSION );

	m_packet.WriteString( RString(PRODUCT_ID_VER) );

	/* Block until response is received.
	 * Move mode to blocking in order to give CPU back to the system,
	 * and not wait.
	 */

	bool dontExit = true;

	NetPlayerClient->blocking = true;

	// The following packet must get through, so we block for it.
	// If we are serving, we do not block for this.
	NetPlayerClient->SendPack( (char*)m_packet.Data, m_packet.Position );

	m_packet.Clear();

	while( dontExit )
	{
		m_packet.Clear();
		if( NetPlayerClient->ReadPack((char *)&m_packet, NETMAXBUFFERSIZE)<1 )
			dontExit=false; // Also allow exit if there is a problem on the socket
		if( m_packet.Read1() == NSServerOffset + NSCHello )
			dontExit=false;
		// Only allow passing on handshake; otherwise scoreboard updates and
		// such will confuse us.
	}

	NetPlayerClient->blocking = false;

	m_ServerVersion = m_packet.Read1();
	if( m_ServerVersion >= 128 )
		isSMOnline = true;

	m_ServerName = m_packet.ReadString();
	m_iSalt = m_packet.Read4();
	LOG->Info( "Server Version: %d %s", m_ServerVersion, m_ServerName.c_str() );
}


void NetworkSyncManager::StartUp()
{
	RString ServerIP;

	if( GetCommandlineArgument( "netip", &ServerIP ) )
		PostStartUp( ServerIP );

	BroadcastReception = new EzSockets;
	BroadcastReception->create( IPPROTO_UDP );
	BroadcastReception->bind( 8765 );
	BroadcastReception->blocking = false;
}


bool NetworkSyncManager::Connect( const RString& addy, unsigned short port )
{
	LOG->Info( "Beginning to connect" );

	NetPlayerClient->create(); // Initialize Socket
	useSMserver = NetPlayerClient->connect( addy, port );
    
	return useSMserver;
}

void NetworkSyncManager::ReportNSSOnOff(int i) 
{
	m_packet.Clear();
	m_packet.Write1( NSCSMS );
	m_packet.Write1( (uint8_t) i );
	NetPlayerClient->SendPack( (char*)m_packet.Data, m_packet.Position );
}

RString NetworkSyncManager::GetServerName() 
{ 
	return m_ServerName;
}

void NetworkSyncManager::ReportScore(int playerID, int step, int score, int combo, float offset)
{
	if( !useSMserver ) //Make sure that we are using the network
		return;

	LOG->Trace( ssprintf("Player ID %i combo = %i", playerID, combo) );
	m_packet.Clear();

	m_packet.Write1( NSCGSU );
	step = TranslateStepType(step);
	uint8_t ctr = (uint8_t) (playerID * 16 + step - ( SMOST_HITMINE - 1 ) );
	m_packet.Write1( ctr );

	ctr = uint8_t( STATSMAN->m_CurStageStats.m_player[playerID].GetGrade()*16 );

	if ( STATSMAN->m_CurStageStats.m_player[playerID].m_bFailed )
		ctr = uint8_t( 112 );	//Code for failed (failed constant seems not to work)

	m_packet.Write1( ctr );
	m_packet.Write4( score );
	m_packet.Write2( (uint16_t)combo );
	m_packet.Write2( (uint16_t)m_playerLife[playerID] );

	// Offset Info
	// Note: if a 0 is sent, then disregard data.

	// ASSUMED: No step will be more than 16 seconds off center.
	// If this assumption is false, read 16 seconds in either direction.
	int iOffset = int( (offset+16.384)*2000.0f );

	if( iOffset>65535 )
		iOffset=65535;
	if( iOffset<1 )
		iOffset=1;

	// Report 0 if hold, or miss (don't forget mines should report)
	if( step == SMOST_HITMINE || step > SMOST_W1 )
		iOffset = 0;

	m_packet.Write2( (uint16_t)iOffset );

	NetPlayerClient->SendPack( (char*)m_packet.Data, m_packet.Position ); 

}

void NetworkSyncManager::ReportSongOver() 
{
	if ( !useSMserver )	//Make sure that we are using the network
		return;

	m_packet.Clear();

	m_packet.Write1( NSCGON );

	NetPlayerClient->SendPack( (char*)&m_packet.Data, m_packet.Position ); 
	return;
}

void NetworkSyncManager::ReportStyle() 
{
	LOG->Trace( "Sending \"Style\" to server" );

	if( !useSMserver )
		return;
	m_packet.Clear();
	m_packet.Write1( NSCSU );
	m_packet.Write1( (int8_t)GAMESTATE->GetNumPlayersEnabled() );

	FOREACH_EnabledPlayer( pn ) 
	{
		m_packet.Write1( (uint8_t)pn );
		m_packet.WriteString( GAMESTATE->GetPlayerDisplayName(pn) );
	}

	NetPlayerClient->SendPack( (char*)&m_packet.Data, m_packet.Position );
}

void NetworkSyncManager::StartRequest( short position ) 
{
	if( !useSMserver )
		return;

	if( GAMESTATE->m_bDemonstrationOrJukebox )
		return;

	LOG->Trace( "Requesting Start from Server." );

	m_packet.Clear();

	m_packet.Write1( NSCGSR );

	unsigned char ctr=0;

	Steps * tSteps;
	tSteps = GAMESTATE->m_pCurSteps[PLAYER_1];
	if( tSteps!=NULL && GAMESTATE->IsPlayerEnabled(PLAYER_1) )
		ctr = uint8_t(ctr+tSteps->GetMeter()*16);

	tSteps = GAMESTATE->m_pCurSteps[PLAYER_2];
	if( tSteps!=NULL && GAMESTATE->IsPlayerEnabled(PLAYER_2) )
		ctr = uint8_t( ctr+tSteps->GetMeter() );

	m_packet.Write1( ctr );

	ctr=0;

	tSteps = GAMESTATE->m_pCurSteps[PLAYER_1];
	if( tSteps!=NULL && GAMESTATE->IsPlayerEnabled(PLAYER_1) )
		ctr = uint8_t( ctr + (int)tSteps->GetDifficulty()*16 );

	tSteps = GAMESTATE->m_pCurSteps[PLAYER_2];
	if( tSteps!=NULL && GAMESTATE->IsPlayerEnabled(PLAYER_2) )
		ctr = uint8_t( ctr + (int)tSteps->GetDifficulty() );

	m_packet.Write1( ctr );

	//Notify server if this is for sync or not.
	ctr = char( position*16 );
	m_packet.Write1( ctr );

	if( GAMESTATE->m_pCurSong != NULL )
	{
		m_packet.WriteString( GAMESTATE->m_pCurSong->m_sMainTitle );
		m_packet.WriteString( GAMESTATE->m_pCurSong->m_sSubTitle );
		m_packet.WriteString( GAMESTATE->m_pCurSong->m_sArtist );
	}
	else
	{
		m_packet.WriteString( "" );
		m_packet.WriteString( "" );
		m_packet.WriteString( "" );
	}

	if( GAMESTATE->m_pCurCourse != NULL )
		m_packet.WriteString( GAMESTATE->m_pCurCourse->GetDisplayFullTitle() );
	else
		m_packet.WriteString( RString() );

	//Send Player (and song) Options
	m_packet.WriteString( GAMESTATE->m_SongOptions.GetCurrent().GetString() );

	int players=0;
	FOREACH_PlayerNumber( p )
	{
		++players;
		m_packet.WriteString( GAMESTATE->m_pPlayerState[p]->m_PlayerOptions.GetCurrent().GetString() );
	}
	for (int i=0; i<2-players; ++i)
		m_packet.WriteString("");	//Write a NULL if no player

	//This needs to be reset before ScreenEvaluation could possibly be called
	m_EvalPlayerData.clear();

	//Block until go is recieved.
	//Switch to blocking mode (this is the only
	//way I know how to get precievably instantanious results

	bool dontExit=true;

	NetPlayerClient->blocking=true;

	//The following packet HAS to get through, so we turn blocking on for it as well
	//Don't block if we are serving
	NetPlayerClient->SendPack((char*)&m_packet.Data, m_packet.Position); 
	
	LOG->Trace("Waiting for RECV");

	m_packet.Clear();

	while (dontExit)
	{
		m_packet.Clear();
		if (NetPlayerClient->ReadPack((char *)&m_packet, NETMAXBUFFERSIZE)<1)
				dontExit=false; // Also allow exit if there is a problem on the socket
								// Only do if we are not the server, otherwise the sync
								// gets hosed up due to non blocking mode.

			if (m_packet.Read1() == (NSServerOffset + NSCGSR))
				dontExit=false;
		// Only allow passing on Start request; otherwise scoreboard updates
		// and such will confuse us.

	}
	NetPlayerClient->blocking=false;

}

static LocalizedString CONNECTION_SUCCESSFUL( "NetworkSyncManager", "Connection to '%s' successful." );
static LocalizedString CONNECTION_FAILED	( "NetworkSyncManager", "Connection failed." );
void NetworkSyncManager::DisplayStartupStatus()
{
	RString sMessage("");

	switch (m_startupStatus)
	{
	case 0:
		//Networking wasn't attepmpted
		return;
	case 1:
		sMessage = ssprintf( CONNECTION_SUCCESSFUL.GetValue(), m_ServerName.c_str() );
		break;
	case 2:
		sMessage = CONNECTION_FAILED.GetValue();
		break;
	}
	SCREENMAN->SystemMessage( sMessage );
}

void NetworkSyncManager::Update(float fDeltaTime)
{
	if (useSMserver)
		ProcessInput();

	NetworkPacket BroadIn;
	if ( BroadcastReception->ReadPack( (char*)&BroadIn.Data, 1020 ) )
	{
		NetServerInfo ThisServer;
		BroadIn.Position = 0;
		if ( BroadIn.Read1() == 141 ) // SMLS_Info?
		{
			ThisServer.Name = BroadIn.ReadString();
			int port = BroadIn.Read2();
			BroadIn.Read2();	//Num players connected.
			uint32_t addy = EzSockets::LongFromAddrIn(BroadcastReception->fromAddr);
			ThisServer.Address = ssprintf( "%u.%u.%u.%u:%d",
				(addy<<0)>>24, (addy<<8)>>24, (addy<<16)>>24, (addy<<24)>>24, port );

			//It's fairly safe to assume that users will not be on networks with more than
			//30 or 40 servers.  Until this point, maps would be slower than vectors. 
			//So I am going to use a vector to store all of the servers.  
			//
			//In this situation, I will traverse the vector to find the element that 
			//contains the corresponding server.

			unsigned int i;
			for ( i = 0; i < m_vAllLANServers.size(); i++ )
			{
				if ( m_vAllLANServers[i].Address == ThisServer.Address )
				{
					m_vAllLANServers[i].Name = ThisServer.Name;
					break;
				}
			}
			if ( i >= m_vAllLANServers.size() )
				m_vAllLANServers.push_back( ThisServer );
		}
	}
}

static LocalizedString CONNECTION_DROPPED( "NetworkSyncManager", "Connection to server dropped." );
void NetworkSyncManager::ProcessInput()
{
	//If we're disconnected, just exit
	if ((NetPlayerClient->state!=NetPlayerClient->skCONNECTED) || 
			NetPlayerClient->IsError())
	{
		SCREENMAN->SystemMessageNoAnimate( CONNECTION_DROPPED );
		useSMserver=false;
		isSMOnline = false;
		FOREACH_PlayerNumber(pn)
			isSMOLoggedIn[pn] = false;
		NetPlayerClient->close();
		return;
	}

	// load new data into buffer
	NetPlayerClient->update();
	m_packet.Clear();

	int packetSize;
	while ( (packetSize = NetPlayerClient->ReadPack((char *)&m_packet, NETMAXBUFFERSIZE) ) > 0 )
	{
		m_packet.Size = packetSize;
		int command = m_packet.Read1();
		//Check to make sure command is valid from server
		if (command < NSServerOffset)
		{
			LOG->Trace("CMD (below 128) Invalid> %d",command);
 			break;
		}

		command = command - NSServerOffset;

		switch (command)
		{
		case NSCPing: // Ping packet responce
			m_packet.Clear();
			m_packet.Write1( NSCPingR );
			NetPlayerClient->SendPack((char*)m_packet.Data,m_packet.Position);
			break;
		case NSCPingR: // These are in response to when/if we send packet 0's
		case NSCHello: // This is already taken care of by the blocking code earlier
		case NSCGSR:   // This is taken care of by the blocking start code
			break;
		case NSCGON: 
			{
				int PlayersInPack = m_packet.Read1();
				m_EvalPlayerData.resize(PlayersInPack);
				for (int i=0; i<PlayersInPack; ++i)
					m_EvalPlayerData[i].name = m_packet.Read1();
				for (int i=0; i<PlayersInPack; ++i)
					m_EvalPlayerData[i].score = m_packet.Read4();
				for (int i=0; i<PlayersInPack; ++i)
					m_EvalPlayerData[i].grade = m_packet.Read1();
				for (int i=0; i<PlayersInPack; ++i)
					m_EvalPlayerData[i].difficulty = (Difficulty) m_packet.Read1();
				for (int j=0; j<NETNUMTAPSCORES; ++j) 
					for (int i=0; i<PlayersInPack; ++i)
						m_EvalPlayerData[i].tapScores[j] = m_packet.Read2();
				for (int i=0; i<PlayersInPack; ++i)
					m_EvalPlayerData[i].playerOptions = m_packet.ReadString();
				SCREENMAN->SendMessageToTopScreen( SM_GotEval );
			}
			break;
		case NSCGSU: // Scoreboard Update
			{	//Ease scope
				int ColumnNumber=m_packet.Read1();
				int NumberPlayers=m_packet.Read1();
				RString ColumnData;

				switch (ColumnNumber)
				{
				case NSSB_NAMES:
					ColumnData = "Names\n";
					for (int i=0; i<NumberPlayers; ++i)
						ColumnData += m_PlayerNames[m_packet.Read1()] + "\n";
					break;
				case NSSB_COMBO:
					ColumnData = "Combo\n";
					for (int i=0; i<NumberPlayers; ++i)
						ColumnData += ssprintf("%d\n",m_packet.Read2());
					break;
				case NSSB_GRADE:
					ColumnData = "Grade\n";
					for (int i=0;i<NumberPlayers;i++)
						ColumnData += GradeToLocalizedString( Grade(m_packet.Read1()) ) + "\n";
					break;
				}
				m_Scoreboard[ColumnNumber] = ColumnData;
				m_scoreboardchange[ColumnNumber]=true;
				/*
				Message msg("ScoreboardUpdate");
				msg.SetParam( "NumPlayers", NumberPlayers );
				MESSAGEMAN->Broadcast( msg );
				*/
			}
			break;
		case NSCSU: //System message from server
			{
				RString SysMSG = m_packet.ReadString();
				SCREENMAN->SystemMessage( SysMSG );
			}
			break;
		case NSCCM: //Chat message from server
			{
				m_sChatText += m_packet.ReadString() + " \n ";
				//10000 chars backlog should be more than enough
				m_sChatText = m_sChatText.Right(10000);
				SCREENMAN->SendMessageToTopScreen( SM_AddToChat );
			}
			break;
		case NSCRSG: //Select Song/Play song
			{
				m_iSelectMode = m_packet.Read1();
				m_sMainTitle = m_packet.ReadString();
				m_sArtist = m_packet.ReadString();
				m_sSubTitle = m_packet.ReadString();
				SCREENMAN->SendMessageToTopScreen( SM_ChangeSong );
			}
			break;
		case NSCUUL:
			{
				/*int ServerMaxPlayers=*/m_packet.Read1();
				int PlayersInThisPacket=m_packet.Read1();
				m_ActivePlayer.clear();
				m_PlayerStatus.clear();
				m_PlayerNames.clear();
				m_ActivePlayers = 0;
				for (int i=0; i<PlayersInThisPacket; ++i)
				{
					int PStatus = m_packet.Read1();
					if ( PStatus > 0 )
					{
						m_ActivePlayers++;
						m_ActivePlayer.push_back( i );
					}
					m_PlayerStatus.push_back( PStatus );
					m_PlayerNames.push_back( m_packet.ReadString() );
				}
				SCREENMAN->SendMessageToTopScreen( SM_UsersUpdate );
			}
			break;
		case NSCSMS:
			{
				RString StyleName, GameName;
				GameName = m_packet.ReadString();
				StyleName = m_packet.ReadString();

				GAMESTATE->SetCurGame( GAMEMAN->StringToGame(GameName) );
				GAMESTATE->SetCurrentStyle( GAMEMAN->GameAndStringToStyle(GAMESTATE->m_pCurGame,StyleName) );

				SCREENMAN->SetNewScreen( "ScreenNetSelectMusic" ); //Should this be metric'd out?
			}
			break;
		case NSCSMOnline:
			{
				m_SMOnlinePacket.Size = packetSize - 1;
				m_SMOnlinePacket.Position = 0;
				memcpy( m_SMOnlinePacket.Data, (m_packet.Data + 1), packetSize-1 );
				LOG->Trace( "Received SMOnline Command: %d, size:%d", command, packetSize - 1 );
				SCREENMAN->SendMessageToTopScreen( SM_SMOnlinePack );
			}
			break;
		case NSCAttack:
			{
				PlayerNumber iPlayerNumber = (PlayerNumber)m_packet.Read1();

				if( GAMESTATE->IsPlayerEnabled( iPlayerNumber ) ) // Only attack if the player can be attacked.
				{
					Attack a;
					a.fSecsRemaining = float( m_packet.Read4() ) / 1000.0f;
					a.bGlobal = false;
					a.sModifiers = m_packet.ReadString();
					GAMESTATE->m_pPlayerState[iPlayerNumber]->LaunchAttack( a );
				}
				m_packet.Clear();
			}
			break;
		}
		m_packet.Clear();
	}
}

bool NetworkSyncManager::ChangedScoreboard(int Column) 
{
	if (!m_scoreboardchange[Column])
		return false;
	m_scoreboardchange[Column]=false;
	return true;
}

void NetworkSyncManager::SendChat(const RString& message) 
{
	m_packet.Clear();
	m_packet.Write1( NSCCM );
	m_packet.WriteString( message );
	NetPlayerClient->SendPack((char*)&m_packet.Data, m_packet.Position); 
}

void NetworkSyncManager::ReportPlayerOptions()
{
	m_packet.Clear();
	m_packet.Write1( NSCUPOpts );
	FOREACH_PlayerNumber (pn)
		m_packet.WriteString( GAMESTATE->m_pPlayerState[pn]->m_PlayerOptions.GetCurrent().GetString() );
	NetPlayerClient->SendPack((char*)&m_packet.Data, m_packet.Position); 
}

void NetworkSyncManager::SelectUserSong()
{
	m_packet.Clear();
	m_packet.Write1( NSCRSG );
	m_packet.Write1( (uint8_t) m_iSelectMode );
	m_packet.WriteString( m_sMainTitle );
	m_packet.WriteString( m_sArtist );
	m_packet.WriteString( m_sSubTitle );
	NetPlayerClient->SendPack( (char*)&m_packet.Data, m_packet.Position );
}

void NetworkSyncManager::SendSMOnline( )
{
	m_packet.Position = m_SMOnlinePacket.Position + 1;
	memcpy( (m_packet.Data + 1), m_SMOnlinePacket.Data, m_SMOnlinePacket.Position );
	m_packet.Data[0] = NSCSMOnline;
	NetPlayerClient->SendPack( (char*)&m_packet.Data , m_packet.Position );
}

SMOStepType NetworkSyncManager::TranslateStepType(int score)
{
	/* Translate from Stepmania's constantly changing TapNoteScore
	 * to SMO's note scores */
	switch(score)
	{
	case TNS_HitMine:
		return SMOST_HITMINE;
	case TNS_AvoidMine:
		return SMOST_AVOIDMINE;
	case TNS_Miss:
		return SMOST_MISS;
	case TNS_W5:
		return SMOST_W5;
	case TNS_W4:
		return SMOST_W4;
	case TNS_W3:
		return SMOST_W3;
	case TNS_W2:
		return SMOST_W2;
	case TNS_W1:
		return SMOST_W1;
	case HNS_LetGo+TapNoteScore_Invalid:
		return SMOST_LETGO;
	case HNS_Held+TapNoteScore_Invalid:
		return SMOST_HELD;
	default:
		return SMOST_UNUSED;
	}
}

RString NetworkSyncManager::MD5Hex( const RString &sInput ) 
{
	return BinaryToHex( CryptManager::GetMD5ForString(sInput) ).MakeUpper();
}

void NetworkSyncManager::GetListOfLANServers( vector<NetServerInfo>& AllServers ) 
{
	AllServers = m_vAllLANServers;
}

static bool ConnectToServer( const RString &t ) 
{ 
	NSMAN->PostStartUp( t );
	NSMAN->DisplayStartupStatus(); 
	return true;
}

extern Preference<RString> g_sLastServer;

LuaFunction( ConnectToServer, 		ConnectToServer( ( RString(SArg(1)).length()==0 ) ? RString(g_sLastServer) : RString(SArg(1) ) ) )

#endif

static bool ReportStyle() { NSMAN->ReportStyle(); return true; }
static bool CloseNetworkConnection() { NSMAN->CloseConnection(); return true; }

LuaFunction( IsSMOnlineLoggedIn,	NSMAN->isSMOLoggedIn[Enum::Check<PlayerNumber>(L, 1)] )
LuaFunction( IsNetConnected,		NSMAN->useSMserver )
LuaFunction( IsNetSMOnline,			NSMAN->isSMOnline )
LuaFunction( ReportStyle,			ReportStyle() )
LuaFunction( GetServerName,			NSMAN->GetServerName() )
LuaFunction( CloseConnection,		CloseNetworkConnection() )

/*
 * (c) 2003-2004 Charles Lohr, Joshua Allen
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
