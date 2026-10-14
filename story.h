// Undercover story sequencer: state 0xB3 of Driv3r.exe (creator 0x5580e0,
// registered at 0x6df275; object 0x1018 bytes, global 0x8ac4fc).
//
// The front end's "NewGame" (0x553a8f), "ContinueGame" (0x553ac8) and
// "runmission=" / StartGame / fe_dgstart (0x557d70) start it. It walks the
// story table 0x7a5878 (missions, recap stops, progress steps, end videos)
// and sends each mission to gameplay (state 0xB4). Gameplay answers with the
// messages "complete", "fail", "frontend" or "restart" (handler 0x557de0).
//
// This file has no dependency on main.cpp: everything the sequencer does to
// the rest of the program goes through StoryHost.

#pragma once
#include <cstddef>
#include <cstdint>

// What the sequencer needs from the program. Offsets are settings offsets
// (the profile block starts at settings +0x8).
class StoryHost
{
public:
    virtual ~StoryHost() {}
    virtual int32_t ProfileInt(int off) const = 0;
    virtual void SetProfileInt(int off, int32_t v) = 0;
    virtual void SetProfileByte(int off, uint8_t v) = 0;
    // Settings byte +0x5, set while a replay runs (0x547e10): no progress is
    // stored (the cheat codes test the same byte).
    virtual bool ProgressLocked() const = 0;
    // Game setup 0x8ac3ac vtbl+0x24 (0x481720): the level id it was last
    // given (+4), asked by "complete" (through 0x8ac560 vtbl+0x24).
    virtual int SetupLevel() const = 0;
    // 0x557e45: the 0xc90-byte block +0x8 copied to +0xc98.
    virtual void CopyProfileBlock() = 0;
    // 0x558080: missions 5, 11, 15, 17, 22 and 28 run 0x5949d0 on 0x8ac2ac
    // (slots marked 3 of its 128 freed).
    virtual void FreeSlots(int mission) = 0;
    // Game setup 0x8ac3ac vtbl+8 (0x463480): mission id -> level, loads
    // Missions\Mission%.2d.mpc.
    virtual void SetupMission(int mission) = 0;
    // Phase 0 (0x557c37): 0x8ac584 vtbl+8; 0x4d9080 starts the replay
    // recording 0x8ac444 for the level (header: level id, Take A Ride
    // options; the 0x1000-byte block; settings +0x174..+0x207); then state
    // 0xB4 (gameplay).
    virtual void StartGameplay(const uint8_t* block, size_t size) = 0;
    // Phases 1 / 2 (0x557c74 / 0x557cb3): movie player 0x8ac574 vtbl+0x20
    // (path, 1, 2 s before Skip works, stop time or -1) and the screen
    // manager to 0xD1. The program calls VideoDone() when it ends.
    virtual void PlayVideo(const char* path, float stopSeconds) = 0;
    // Phase 4 (0x557ce3): 0x4d92e0(0), the player's context back to 0 (the
    // front end's; a replay runs in context 2).
    virtual void MissionFinished() = 0;
    virtual void GotoSequencer() = 0;  // manager vtbl+0x2c(0xB3)
    virtual void GotoFrontEnd() = 0;   // manager vtbl+0x2c(0xAD)
    virtual void Log(const char* text) = 0;
};

class StorySequencer
{
public:
    // +0x10 phase; Update (vtbl+4, 0x557c20) switches on it.
    // PLAYING (3) is set once gameplay has started (0x557c6a) and again by
    // "restart"; if the sequencer runs again in it, it goes to the front end.
    enum Phase { MISSION = 0, RECAP = 1, VIDEO = 2, PLAYING = 3, FINISHED = 4, QUIT = 5 };

    explicit StorySequencer(StoryHost& host) : host_(host) {}

    void ClearBlock();                 // front end: +0x18, 0x1000 bytes, zeroed
    void NewGame();                    // 0x557d10
    void ContinueGame();               // 0x557d30
    void RunMission(int id);           // 0x557d70 (edx = mission or level id)

    void Update();                     // 0x557c20, while state 0xB3 is current
    void OnMessage(const char* msg);   // 0x557de0
    void VideoDone();                  // movie listener 0x557f00

    int Phase() const { return phase_; }
    int Position() const { return pos_; }
    // Mission id of the current table entry (0 when it is not a mission).
    int CurrentMission() const;

private:
    void Step();                       // 0x557f20
    void Advance();                    // 0x558000
    void RaiseProgress(int value);     // type 3
    void CityStep(int value);          // type 2
    void Logf(const char* fmt, ...);

    StoryHost& host_;
    int phase_ = MISSION;
    int pos_ = 0;                      // +0x14 as an index into the table
    int setupId_ = 0;                  // last id given to the game setup (for the log)
    uint8_t block_[0x1000] = {};       // +0x18
};

// Story missions are played in parts: level ids 0x65..0xb6 are the later
// parts of missions 1..31 (0x4636b0 maps them to the mission's city level).
// Tables of state 0xCC (creator 0x557ad0, object 0x8ac560, vtbl 0x78b9f0):
// vtbl+0x24 0x5579e0: the level is the last part of its mission ("complete"
// stores progress only then; table 0x557a10 for ids 0x66..0xb5).
bool StoryLastPart(int level);
// vtbl+0x20 0x5578f0: the level is followed by another part (table 0x557924
// for ids 1..0xb4); state 0xBB (Update 0x54dab0) then goes to 0xCC.
bool StoryHasNextPart(int level);
// 0xCC Enter 0x5573c0: the next part (jump table 0x557758). toEndState is
// set for 0xb1 -> 0xb2, which goes to state 0xCF instead of back to
// gameplay. Returns 0 when there is none (0x557743: state 0xCF).
int StoryNextPart(int level, bool* toEndState);
