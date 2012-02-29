/* This stores a single note pattern for a song.
 *
 * We can have too much data to keep everything decompressed as NoteData, so most
 * songs are kept in memory compressed as SMData until requested.  NoteData is normally
 * not requested casually during gameplay; we can move through screens, the music
 * wheel, etc. without touching any NoteData.
 *
 * To save more memory, if data is cached on disk, read it from disk on demand.  Not
 * all Steps will have an associated file for this purpose.  (Profile edits don't do
 * this yet.)
 *
 * Data can be on disk (always compressed), compressed in memory, and uncompressed in
 * memory. */
#include "global.h"
#include "Steps.h"
#include "StepsUtil.h"
#include "GameState.h"
#include "Song.h"
#include "RageUtil.h"
#include "RageLog.h"
#include "NoteData.h"
#include "GameManager.h"
#include "SongManager.h"
#include "NoteDataUtil.h"
#include "NotesLoaderSSC.h"
#include "NotesLoaderSM.h"
#include "NotesLoaderSMA.h"
#include "NotesLoaderDWI.h"
#include "NotesLoaderKSF.h"
#include "NotesLoaderBMS.h"
#include <algorithm>

/* register DisplayBPM with StringConversion */
#include "EnumHelper.h"

static const char *DisplayBPMNames[] =
{
	"Actual",
	"Specified",
	"Random",
};
XToString( DisplayBPM );
LuaXType( DisplayBPM );

Steps::Steps(): m_StepsType(StepsType_Invalid), 
	parent(NULL), m_pNoteData(new NoteData), m_bNoteDataIsFilled(false), 
	m_sNoteDataCompressed(""), m_sFilename(""), m_bSavedToDisk(false), 
	m_LoadedFromProfile(ProfileSlot_Invalid), m_iHash(0),
	m_sDescription(""), m_sChartStyle(""), 
	m_Difficulty(Difficulty_Invalid), m_iMeter(0),
	m_bAreCachedRadarValuesJustLoaded(false),
	m_sCredit(""), displayBPMType(DISPLAY_BPM_ACTUAL),
	specifiedBPMMin(0), specifiedBPMMax(0) {}

Steps::~Steps()
{
}

void Steps::GetDisplayBpms( DisplayBpms &AddTo ) const
{
	if( this->GetDisplayBPM() == DISPLAY_BPM_SPECIFIED )
	{
		AddTo.Add( this->GetMinBPM() );
		AddTo.Add( this->GetMaxBPM() );
	}
	else
	{
		float fMinBPM, fMaxBPM;
		this->m_Timing.GetActualBPM( fMinBPM, fMaxBPM );
		AddTo.Add( fMinBPM );
		AddTo.Add( fMaxBPM );
	}
}

bool Steps::HasAttacks() const
{
	return !this->m_Attacks.empty();
}

unsigned Steps::GetHash() const
{
	if( parent )
		return parent->GetHash();
	if( m_iHash )
		return m_iHash;
	if( m_sNoteDataCompressed.empty() )
	{
		if( !m_bNoteDataIsFilled )
			return 0; // No data, no hash.
		NoteDataUtil::GetSMNoteDataString( *m_pNoteData, m_sNoteDataCompressed );
	}
	m_iHash = GetHashForString( m_sNoteDataCompressed );
	return m_iHash;
}

bool Steps::IsNoteDataEmpty() const
{
	return this->m_sNoteDataCompressed.empty();
}

bool Steps::GetNoteDataFromSimfile()
{
	// Replace the line below with the Steps' cache file.
	RString stepFile = this->GetFilename();
	RString extension = GetExtension(stepFile);
	extension.MakeLower(); // must do this because the code is expecting lowercase

	if (extension.empty() || extension == "ssc") // remember cache files.
	{
		SSCLoader loader;
		return loader.LoadNoteDataFromSimfile(stepFile, *this);
	}
	else if (extension == "sm")
	{
		SMLoader loader;
		return loader.LoadNoteDataFromSimfile(stepFile, *this);
	}
	else if (extension == "sma")
	{
		SMALoader loader;
		return loader.LoadNoteDataFromSimfile(stepFile, *this);
	}
	else if (extension == "dwi")
	{
		return DWILoader::LoadNoteDataFromSimfile(stepFile, *this);
	}
	else if (extension == "ksf")
	{
		return KSFLoader::LoadNoteDataFromSimfile(stepFile, *this);
	}
	else if (extension == "bms" || extension == "bml" || extension == "bme" || extension == "pms")
	{
		return BMSLoader::LoadNoteDataFromSimfile(stepFile, *this);
	}
	return false;
}

void Steps::SetNoteData( const NoteData& noteDataNew )
{
	ASSERT( noteDataNew.GetNumTracks() == GAMEMAN->GetStepsTypeInfo(m_StepsType).iNumTracks );

	DeAutogen( false );

	*m_pNoteData = noteDataNew;
	m_bNoteDataIsFilled = true;
	
	m_sNoteDataCompressed = RString();
	m_iHash = 0;
}

void Steps::GetNoteData( NoteData& noteDataOut ) const
{
	Decompress();

	if( m_bNoteDataIsFilled )
	{
		noteDataOut = *m_pNoteData;
	}
	else
	{
		noteDataOut.ClearAll();
		noteDataOut.SetNumTracks( GAMEMAN->GetStepsTypeInfo(m_StepsType).iNumTracks );
	}
}

NoteData Steps::GetNoteData() const
{
	NoteData tmp;
	this->GetNoteData( tmp );
	return tmp;
}

void Steps::SetSMNoteData( const RString &notes_comp_ )
{
	m_pNoteData->Init();
	m_bNoteDataIsFilled = false;

	m_sNoteDataCompressed = notes_comp_;
	m_iHash = 0;
}

/* XXX: this function should pull data from m_sFilename, like Decompress() */
void Steps::GetSMNoteData( RString &notes_comp_out ) const
{
	if( m_sNoteDataCompressed.empty() )
	{
		if( !m_bNoteDataIsFilled ) 
		{
			/* no data is no data */
			notes_comp_out = "";
			return;
		}

		NoteDataUtil::GetSMNoteDataString( *m_pNoteData, m_sNoteDataCompressed );
	}

	notes_comp_out = m_sNoteDataCompressed;
}

float Steps::PredictMeter() const
{
	float pMeter = 0.775f;

	const float RadarCoeffs[NUM_RadarCategory] =
	{
		10.1f, 5.27f,-0.905f, -1.10f, 2.86f,
		0,0,0,0,0,0,0,0
	};
	const RadarValues &rv = GetRadarValues( PLAYER_1 );
	for( int r = 0; r < NUM_RadarCategory; ++r )
		pMeter += rv[r] * RadarCoeffs[r];

	const float DifficultyCoeffs[NUM_Difficulty] =
	{
		-0.877f, -0.877f, 0, 0.722f, 0.722f, 0
	};
	pMeter += DifficultyCoeffs[this->GetDifficulty()];

	// Init non-radar values
	const float SV = rv[RadarCategory_Stream] * rv[RadarCategory_Voltage];
	const float ChaosSquare = rv[RadarCategory_Chaos] * rv[RadarCategory_Chaos];
	pMeter += -6.35f * SV;
	pMeter += -2.58f * ChaosSquare;
	if (pMeter < 1) pMeter = 1;
	return pMeter;
}

void Steps::TidyUpData()
{
	if( m_StepsType == StepsType_Invalid )
		m_StepsType = StepsType_dance_single;

	if( GetDifficulty() == Difficulty_Invalid )
		SetDifficulty( StringToDifficulty(GetDescription()) );

	if( GetDifficulty() == Difficulty_Invalid )
	{
		if(	 GetMeter() == 1 )	SetDifficulty( Difficulty_Beginner );
		else if( GetMeter() <= 3 )	SetDifficulty( Difficulty_Easy );
		else if( GetMeter() <= 6 )	SetDifficulty( Difficulty_Medium );
		else				SetDifficulty( Difficulty_Hard );
	}

	if( GetMeter() < 1) // meter is invalid
		SetMeter( int(PredictMeter()) );
}

void Steps::CalculateRadarValues( float fMusicLengthSeconds )
{
	// If we're autogen, don't calculate values.  GetRadarValues will take from our parent.
	if( parent != NULL )
		return;

	if( m_bAreCachedRadarValuesJustLoaded )
	{
		m_bAreCachedRadarValuesJustLoaded = false;
		return;
	}

	// Do write radar values, and leave it up to the reading app whether they want to trust
	// the cached values without recalculating them.
	/*
	// If we're an edit, leave the RadarValues invalid.
	if( IsAnEdit() )
		return;
	*/

	NoteData tempNoteData;
	this->GetNoteData( tempNoteData );

	FOREACH_PlayerNumber( pn )
		m_CachedRadarValues[pn].Zero();

	if( tempNoteData.IsComposite() ) // TODO: Would it be better to just check the StepsTypeCategory?
	{
		vector<NoteData> vParts;

		NoteDataUtil::SplitCompositeNoteData( tempNoteData, vParts );
		for( size_t pn = 0; pn < min(vParts.size(), size_t(NUM_PLAYERS)); ++pn )
			NoteDataUtil::CalculateRadarValues( vParts[pn], fMusicLengthSeconds, m_CachedRadarValues[pn] );
	}
	else if (this->GetStepsTypeCategory() == StepsTypeCategory_Couple)
	{
		NoteData p1 = tempNoteData;
		// XXX: Assumption that couple will always have an even number of notes.
		const int tracks = tempNoteData.GetNumTracks() / 2;
		p1.SetNumTracks(tracks);
		NoteDataUtil::CalculateRadarValues(p1,
										   fMusicLengthSeconds,
										   m_CachedRadarValues[PLAYER_1]);
		// at this point, p2 is tempNoteData.
		NoteDataUtil::ShiftTracks(tempNoteData, tracks);
		tempNoteData.SetNumTracks(tracks);
		NoteDataUtil::CalculateRadarValues(tempNoteData,
										   fMusicLengthSeconds,
										   m_CachedRadarValues[PLAYER_2]);
	}
	else
	{
		NoteDataUtil::CalculateRadarValues( tempNoteData, fMusicLengthSeconds, m_CachedRadarValues[0] );
		fill_n( m_CachedRadarValues + 1, NUM_PLAYERS-1, m_CachedRadarValues[0] );
	}
}

void Steps::Decompress() const
{
	const_cast<Steps *>(this)->Decompress();
}

void Steps::Decompress()
{
	if( m_bNoteDataIsFilled )
		return;	// already decompressed

	if( parent )
	{
		// get autogen m_pNoteData
		NoteData notedata;
		parent->GetNoteData( notedata );

		m_bNoteDataIsFilled = true;

		int iNewTracks = GAMEMAN->GetStepsTypeInfo(m_StepsType).iNumTracks;

		if( this->m_StepsType == StepsType_lights_cabinet )
		{
			NoteDataUtil::LoadTransformedLights( notedata, *m_pNoteData, iNewTracks );
		}
		else
		{
			NoteDataUtil::LoadTransformedSlidingWindow( notedata, *m_pNoteData, iNewTracks );

			NoteDataUtil::RemoveStretch( *m_pNoteData, m_StepsType );
		}
		return;
	}

	if( !m_sFilename.empty() && m_sNoteDataCompressed.empty() )
	{
		// We have NoteData on disk and not in memory. Load it.
		if (!this->GetNoteDataFromSimfile())
		{
			LOG->Warn("Couldn't load the %s chart's NoteData from \"%s\"",
					  DifficultyToString(m_Difficulty).c_str(), m_sFilename.c_str());
			return;
		}

		this->GetSMNoteData( m_sNoteDataCompressed );
	}

	if( m_sNoteDataCompressed.empty() )
	{
		/* there is no data, do nothing */
	}
	else
	{
		// load from compressed
		bool bComposite = this->GetStepsTypeCategory() == StepsTypeCategory_Routine;
		m_bNoteDataIsFilled = true;
		m_pNoteData->SetNumTracks( GAMEMAN->GetStepsTypeInfo(m_StepsType).iNumTracks );

		NoteDataUtil::LoadFromSMNoteDataString( *m_pNoteData, m_sNoteDataCompressed, bComposite );
	}
}

void Steps::Compress() const
{
	// Always leave lights data uncompressed.
	if( this->m_StepsType == StepsType_lights_cabinet && m_bNoteDataIsFilled )
	{
		m_sNoteDataCompressed = RString();
		return;
	}
	
	// Don't compress data in the editor: it's still in use.
	if (GAMESTATE->m_bInStepEditor)
	{
		return;
	}

	if( !m_sFilename.empty() && m_LoadedFromProfile == ProfileSlot_Invalid )
	{
		/* We have a file on disk; clear all data in memory.
		 * Data on profiles can't be accessed normally (need to mount and time-out
		 * the device), and when we start a game and load edits, we want to be
		 * sure that it'll be available if the user picks it and pulls the device.
		 * Also, Decompress() doesn't know how to load .edits. */
		m_pNoteData->Init();
		m_bNoteDataIsFilled = false;

		/* Be careful; 'x = ""', m_sNoteDataCompressed.clear() and m_sNoteDataCompressed.reserve(0)
		 * don't always free the allocated memory. */
		m_sNoteDataCompressed = RString();
		return;
	}

	// We have no file on disk. Compress the data, if necessary.
	if( m_sNoteDataCompressed.empty() )
	{
		if( !m_bNoteDataIsFilled )
			return; /* no data is no data */
		NoteDataUtil::GetSMNoteDataString( *m_pNoteData, m_sNoteDataCompressed );
	}

	m_pNoteData->Init();
	m_bNoteDataIsFilled = false;
}

/* Copy our parent's data. This is done when we're being changed from autogen
 * to normal. (needed?) */
void Steps::DeAutogen( bool bCopyNoteData )
{
	if( !parent )
		return; // OK

	if( bCopyNoteData )
		Decompress();	// fills in m_pNoteData with sliding window transform

	m_sDescription		= Real()->m_sDescription;
	m_sChartStyle		= Real()->m_sChartStyle;
	m_Difficulty		= Real()->m_Difficulty;
	m_iMeter		= Real()->m_iMeter;
	copy( Real()->m_CachedRadarValues, Real()->m_CachedRadarValues + NUM_PLAYERS, m_CachedRadarValues );
	m_sCredit		= Real()->m_sCredit;
	parent = NULL;

	if( bCopyNoteData )
		Compress();
}

void Steps::AutogenFrom( const Steps *parent_, StepsType ntTo )
{
	parent = parent_;
	m_StepsType = ntTo;
	m_Timing = parent->m_Timing;
}

void Steps::CopyFrom( Steps* pSource, StepsType ntTo, float fMusicLengthSeconds )	// pSource does not have to be of the same StepsType
{
	m_StepsType = ntTo;
	NoteData noteData;
	pSource->GetNoteData( noteData );
	noteData.SetNumTracks( GAMEMAN->GetStepsTypeInfo(ntTo).iNumTracks );
	parent = NULL;
	m_Timing = pSource->m_Timing;
	this->m_Attacks = pSource->m_Attacks;
	this->m_sAttackString = pSource->m_sAttackString;
	this->SetNoteData( noteData );
	this->SetDescription( pSource->GetDescription() );
	this->SetDifficulty( pSource->GetDifficulty() );
	this->SetMeter( pSource->GetMeter() );
	this->CalculateRadarValues( fMusicLengthSeconds );
}

void Steps::CreateBlank( StepsType ntTo )
{
	m_StepsType = ntTo;
	NoteData noteData;
	noteData.SetNumTracks( GAMEMAN->GetStepsTypeInfo(ntTo).iNumTracks );
	this->SetNoteData( noteData );
}

void Steps::SetDifficultyAndDescription( Difficulty dc, RString sDescription )
{
	DeAutogen();
	m_Difficulty = dc;
	m_sDescription = sDescription;
	if( GetDifficulty() == Difficulty_Edit )
		MakeValidEditDescription( m_sDescription );
}

void Steps::SetCredit( RString sCredit )
{
	DeAutogen();
	m_sCredit = sCredit;
}

void Steps::SetChartStyle( RString sChartStyle )
{
	DeAutogen();
	m_sChartStyle = sChartStyle;
}

bool Steps::MakeValidEditDescription( RString &sPreferredDescription )
{
	if( int(sPreferredDescription.size()) > MAX_STEPS_DESCRIPTION_LENGTH )
	{
		sPreferredDescription = sPreferredDescription.Left( MAX_STEPS_DESCRIPTION_LENGTH );
		return true;
	}
	return false;
}

void Steps::SetMeter( int meter )
{
	DeAutogen();
	m_iMeter = meter;
}

bool Steps::HasSignificantTimingChanges() const
{
	if( m_Timing.HasStops() || m_Timing.HasDelays() )
		return true;
	
	/* TODO: Deal with DisplayBPM here...if possible?
	 * Song's version may still be useful. */
	
	else if( m_Timing.HasBpmChanges() || m_Timing.HasWarps() || m_Timing.HasSpeedChanges() )
	{
		return true;
	}
	return false;
}

void Steps::SetCachedRadarValues( const RadarValues v[NUM_PLAYERS] )
{
	DeAutogen();
	copy( v, v + NUM_PLAYERS, m_CachedRadarValues );
	m_bAreCachedRadarValuesJustLoaded = true;
}

bool Steps::UsesSplitTiming() const
{
	Song *song = SONGMAN->GetSongFromSteps(const_cast<Steps *>(this));
	return song->m_SongTiming != this->m_Timing;
}

vector<int> Steps::GetNumHoldsOfType(const TapNote::SubType holdType, int start, int end) const
{
	vector<int> num(1);
	if (!this->IsMultiPlayerStyle() &&
		!this->m_Timing.HasWarps() &&
		!this->m_Timing.HasFakes())
	{
		num[0] = this->GetNoteData().GetNumHoldsOfType(holdType, start, end);
		return num;
	}
	if (this->IsMultiPlayerStyle())
	{
		num.resize(NUM_PLAYERS);
	}
	const NoteData &nd = this->GetNoteData();
	FOREACH_NONEMPTY_ROW_ALL_TRACKS_RANGE(nd, r, start, end)
	{
		if (!this->m_Timing.IsJudgableAtRow(r))
		{
			continue;
		}
		for (int t = 0; t < nd.GetNumTracks(); ++t)
		{
			const TapNote &tn = nd.GetTapNote(t, r);
			if (tn.type == TapNote::hold_head &&
				tn.subType == holdType)
			{
				++num[this->GetEffectivePlayer(t, tn)];
			}
		}
	}
	return num;
}

StepsType Steps::GetStepsType() const
{
	return this->m_StepsType;
}

StepsTypeCategory Steps::GetStepsTypeCategory() const
{
	return GAMEMAN->GetStepsTypeInfo(this->GetStepsType()).m_StepsTypeCategory;
}

bool Steps::IsMultiPlayerStyle() const
{
	const StepsTypeCategory &cat = this->GetStepsTypeCategory();
	return cat == StepsTypeCategory_Couple || cat == StepsTypeCategory_Routine;
}

PlayerNumber Steps::GetEffectivePlayer(const int track, const TapNote &tn) const
{
	if (!this->IsMultiPlayerStyle())
	{
		return PLAYER_1;
	}
	if (this->GetStepsTypeCategory() == StepsTypeCategory_Routine)
	{
		return tn.pn;
	}
	// at this point, we know it's couple.
	return (track < (this->GetNoteData().GetNumTracks() / 2)) ? PLAYER_1 : PLAYER_2;
}

vector<bool> Steps::RowNeedsAtLeastSimultaneousPresses(int min, int row) const
{
	if (!this->IsMultiPlayerStyle())
	{
		vector<bool> result(1, this->GetNoteData().RowNeedsAtLeastSimultaneousPresses(min, row));
		return result;
	}
	vector<bool> result(NUM_PLAYERS);
	vector<int> found(NUM_PLAYERS);

	const NoteData &nd = this->GetNoteData();
	for (int t = 0; t < nd.GetNumTracks(); ++t)
	{
		const TapNote &tn = nd.GetTapNote(t, row);
		switch (tn.type)
		{
		case TapNote::mine:
		case TapNote::empty:
		case TapNote::fake:
		case TapNote::lift: // you don't "press" on a lift.
		case TapNote::autoKeysound:
			continue; // should attack type notes be included?
		default:
			++found[this->GetEffectivePlayer(t, tn)];
		}
	}
	// if no taps are found for any players, abandon ship.
	bool valid = false;
	FOREACH(int, found, i)
	{
		if (*i > 0)
		{
			valid = true;
			break;
		}
	}
	if (!valid)
	{
		return result; // by default, bools in vectors should be set to false.
	}

	FOREACH(int, found, i)
	{
		if (*i < min)
		{
			valid = false;
			break;
		}
	}
	if (valid)
	{
		vector<bool> yes(result.size(), true);
		return yes; // all players require the min number of taps.
	}

	/*
	 * At this point, at least one player doesn't have to press the minimum number of taps.
	 * Check the holds at this point. Specifically, check the adjacent ones. */
	for (int t = 0; t < nd.GetNumTracks(); ++t)
	{
		int headRow = nd.GetSoonestHoldHeadAtRow(t, row);
		if (headRow != -1)
		{
			++found[this->GetEffectivePlayer(t, nd.GetTapNote(t, headRow))];
		}
	}
	for (unsigned i = 0; i < found.size(); ++i)
	{
		if (found[i] > min)
		{
			result[i] = true;
		}
	}
	return result;
}

vector<int> Steps::GetNumRowsWithSimultaneousPresses(int min, int start, int end) const
{
	vector<int> num(1);
	if (!this->IsMultiPlayerStyle() &&
		!this->m_Timing.HasWarps() &&
		!this->m_Timing.HasFakes())
	{
		num[0] = this->GetNoteData().GetNumRowsWithSimultaneousPresses(min, start, end);
		return num;
	}
	if (this->IsMultiPlayerStyle())
	{
		num.resize(NUM_PLAYERS);
	}
	const NoteData &nd = this->GetNoteData();
	FOREACH_NONEMPTY_ROW_ALL_TRACKS_RANGE(nd, r, start, end)
	{
		if (this->m_Timing.IsJudgableAtRow(r))
		{
			vector<bool> result = this->RowNeedsAtLeastSimultaneousPresses(min, r);
			for (unsigned i = 0; i < num.size(); ++i)
			{
				if (result[i])
				{
					++num[i];
				}
			}
		}
	}
	return num;
}

vector<int> Steps::GetNumRowsWithSimultaneousTaps(int min, int start, int end) const
{
	vector<int> num(1);
	if (!this->IsMultiPlayerStyle() &&
		!this->m_Timing.HasWarps() &&
		!this->m_Timing.HasFakes())
	{
		num[0] = this->GetNoteData().GetNumRowsWithSimultaneousTaps(min, start, end);
		return num;
	}
	if (this->IsMultiPlayerStyle())
	{
		num.resize(NUM_PLAYERS);
	}
	const NoteData &nd = this->GetNoteData();
	FOREACH_NONEMPTY_ROW_ALL_TRACKS_RANGE(nd, r, start, end)
	{
		if (!this->m_Timing.IsJudgableAtRow(r))
		{
			continue;
		}
		vector<int> found(num.size());
		for (int t = 0; t < nd.GetNumTracks(); ++t)
		{
			const TapNote &tn = nd.GetTapNote(t, r);
			if (nd.IsTap(tn))
			{
				++found[this->GetEffectivePlayer(t, tn)];
			}
		}
		for (unsigned i = 0; i < num.size(); ++i)
		{
			if (found[i] >= min)
			{
				++num[i];
			}
		}
	}
	return num;
}

bool Steps::IsTap(const TapNote &tn, const int row) const
{
	if (this->m_Timing.IsJudgableAtRow(row))
		return this->GetNoteData().IsTap(tn);
	return false;
}

bool Steps::IsMine(const TapNote &tn, const int row) const
{
	if (this->m_Timing.IsJudgableAtRow(row))
		return this->GetNoteData().IsMine(tn);
	return false;
}

bool Steps::IsLift(const TapNote &tn, const int row) const
{
	if (this->m_Timing.IsJudgableAtRow(row))
		return this->GetNoteData().IsLift(tn);
	return false;
}

bool Steps::IsFake(const TapNote &tn, const int row) const
{
	if (this->m_Timing.IsJudgableAtRow(row))
		return this->GetNoteData().IsFake(tn);
	return true;
}

vector<int> Steps::GetNumTapsOfType(int start,
		int end,
		int (NoteData::*NumTapType)(int, int) const,
		bool (NoteData::*TapType)(const TapNote &) const
		) const
{
	vector<int> num(1);
	if (!this->IsMultiPlayerStyle() &&
		!this->m_Timing.HasWarps() &&
		!this->m_Timing.HasFakes())
	{
		num[0] = (this->GetNoteData().*NumTapType)(start, end);
		return num;
	}
	if (this->IsMultiPlayerStyle())
	{
		num.resize(NUM_PLAYERS);
	}
	const NoteData &nd = this->GetNoteData();
	FOREACH_NONEMPTY_ROW_ALL_TRACKS_RANGE(nd, r, start, end)
	{
		if (this->m_Timing.IsJudgableAtRow(r))
		{
			for (int t = 0; t < nd.GetNumTracks(); ++t)
			{
				const TapNote &tn = nd.GetTapNote(t, r);
				if ((nd.*TapType)(tn))
				{
					++num[this->GetEffectivePlayer(t, tn)];
				}
			}
		}
	}
	return num;
}

vector<int> Steps::GetNumTapNotes(int start, int end) const
{
	return this->GetNumTapsOfType(start, end, &NoteData::GetNumTapNotes, &NoteData::IsTap);
}

int Steps::GetNumRowsWithTap( int start, int end ) const
{
	if (!this->m_Timing.HasWarps() &&
		!this->m_Timing.HasFakes())
	{
		return this->GetNoteData().GetNumRowsWithTap(start, end);
	}
	int num = 0;
	const NoteData &nd = this->GetNoteData();
	FOREACH_NONEMPTY_ROW_ALL_TRACKS_RANGE(nd, r, start, end)
	{
		if(nd.IsThereATapAtRow(r) && this->m_Timing.IsJudgableAtRow(r))
		{
			++num;
		}
	}

	return num;
}

int Steps::GetNumRowsWithTapOrHoldHead(int start, int end) const
{
	if (!this->m_Timing.HasWarps() &&
		!this->m_Timing.HasFakes())
	{
		return this->GetNoteData().GetNumRowsWithTapOrHoldHead(start, end);
	}
	int num = 0;
	const NoteData &nd = this->GetNoteData();
	FOREACH_NONEMPTY_ROW_ALL_TRACKS_RANGE(nd, r, start, end)
	{
		if (nd.IsThereATapOrHoldHeadAtRow(r) && this->m_Timing.IsJudgableAtRow(r))
		{
			++num;
		}
	}
	return num;
}

vector<int> Steps::GetNumJumps(int start, int end) const
{
	return this->GetNumRowsWithSimultaneousTaps(2, start, end);
}

vector<int> Steps::GetNumHands(int start, int end) const
{
	return this->GetNumRowsWithSimultaneousPresses(3, start, end);
}

vector<int> Steps::GetNumQuads(int start, int end) const
{
	return this->GetNumRowsWithSimultaneousPresses(4, start, end);
}

vector<int> Steps::GetNumHoldNotes(int start, int end) const
{
	return this->GetNumHoldsOfType(TapNote::hold_head_hold, start, end);
}

vector<int> Steps::GetNumRolls(int start, int end) const
{
	return this->GetNumHoldsOfType(TapNote::hold_head_roll, start, end);
}

vector<int> Steps::GetNumMines(int start, int end) const
{
	return this->GetNumTapsOfType(start, end, &NoteData::GetNumMines, &NoteData::IsMine);
}

vector<int> Steps::GetNumLifts(int start, int end) const
{
	return this->GetNumTapsOfType(start, end, &NoteData::GetNumLifts, &NoteData::IsLift);
}

vector<int> Steps::GetNumFakes(int start, int end) const
{
	return this->GetNumTapsOfType(start, end, &NoteData::GetNumFakes, &NoteData::IsFake);
}


// lua start
#include "LuaBinding.h"
/** @brief Allow Lua to have access to the Steps. */
class LunaSteps: public Luna<Steps>
{
public:
	DEFINE_METHOD( GetStepsType,	m_StepsType )
	DEFINE_METHOD( GetDifficulty,	GetDifficulty() )
	DEFINE_METHOD( GetDescription,	GetDescription() )
	DEFINE_METHOD( GetChartStyle,	GetChartStyle() )
	DEFINE_METHOD( GetAuthorCredit, GetCredit() )
	DEFINE_METHOD( GetMeter,	GetMeter() )
	DEFINE_METHOD( GetFilename,	GetFilename() )
	DEFINE_METHOD( IsAutogen,	IsAutogen() )
	DEFINE_METHOD( IsAnEdit,	IsAnEdit() )
	DEFINE_METHOD( IsAPlayerEdit,	IsAPlayerEdit() )
	DEFINE_METHOD( UsesSplitTiming, UsesSplitTiming() )

	static int HasSignificantTimingChanges( T* p, lua_State *L )
	{
		lua_pushboolean(L, p->HasSignificantTimingChanges()); 
		return 1; 
	}
	static int HasAttacks( T* p, lua_State *L )
	{ 
		lua_pushboolean(L, p->HasAttacks()); 
		return 1; 
	}
	static int GetRadarValues( T* p, lua_State *L )
	{
		PlayerNumber pn = Enum::Check<PlayerNumber>(L, 1);
		RadarValues &rv = const_cast<RadarValues &>(p->GetRadarValues(pn));
		rv.PushSelf(L);
		return 1;
	}
	static int GetTimingData( T* p, lua_State *L )
	{
		p->m_Timing.PushSelf(L);
		return 1;
	}
	static int GetHash( T* p, lua_State *L ) { lua_pushnumber( L, p->GetHash() ); return 1; }
	// untested
	/*
	static int GetSMNoteData( T* p, lua_State *L )
	{
		RString out;
		p->GetSMNoteData( out );
		lua_pushstring( L, out );
		return 1;
	}
	*/
	static int GetChartName(T *p, lua_State *L)
	{
		lua_pushstring(L, p->GetChartName());
		return 1;
	}
	static int GetDisplayBpms( T* p, lua_State *L )
	{
		DisplayBpms temp;
		p->GetDisplayBpms(temp);
		float fMin = temp.GetMin();
		float fMax = temp.GetMax();
		vector<float> fBPMs;
		fBPMs.push_back( fMin );
		fBPMs.push_back( fMax );
		LuaHelpers::CreateTableFromArray(fBPMs, L);
		return 1;
	}
	static int IsDisplayBpmSecret( T* p, lua_State *L )
	{
		DisplayBpms temp;
		p->GetDisplayBpms(temp);
		lua_pushboolean( L, temp.IsSecret() );
		return 1;
	}
	static int IsDisplayBpmConstant( T* p, lua_State *L )
	{
		DisplayBpms temp;
		p->GetDisplayBpms(temp);
		lua_pushboolean( L, temp.BpmIsConstant() );
		return 1;
	}
	static int IsDisplayBpmRandom( T* p, lua_State *L )
	{
		lua_pushboolean( L, p->GetDisplayBPM() == DISPLAY_BPM_RANDOM );
		return 1;
	}
	DEFINE_METHOD( PredictMeter, PredictMeter() )
	static int GetDisplayBPMType( T* p, lua_State *L )
	{
		LuaHelpers::Push( L, p->GetDisplayBPM() );
		return 1;
	}

	LunaSteps()
	{
		ADD_METHOD( GetAuthorCredit );
		ADD_METHOD( GetChartStyle );
		ADD_METHOD( GetDescription );
		ADD_METHOD( GetDifficulty );
		ADD_METHOD( GetFilename );
		ADD_METHOD( GetHash );
		ADD_METHOD( GetMeter );
		ADD_METHOD( HasSignificantTimingChanges );
		ADD_METHOD( HasAttacks );
		ADD_METHOD( GetRadarValues );
		ADD_METHOD( GetTimingData );
		ADD_METHOD( GetChartName );
		//ADD_METHOD( GetSMNoteData );
		ADD_METHOD( GetStepsType );
		ADD_METHOD( IsAnEdit );
		ADD_METHOD( IsAutogen );
		ADD_METHOD( IsAPlayerEdit );
		ADD_METHOD( UsesSplitTiming );
		ADD_METHOD( GetDisplayBpms );
		ADD_METHOD( IsDisplayBpmSecret );
		ADD_METHOD( IsDisplayBpmConstant );
		ADD_METHOD( IsDisplayBpmRandom );
		ADD_METHOD( PredictMeter );
		ADD_METHOD( GetDisplayBPMType );
	}
};

LUA_REGISTER_CLASS( Steps )
// lua end


/*
 * (c) 2001-2004 Chris Danford, Glenn Maynard, David Wilson
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
