// Undercover story sequencer (state 0xB3), see story.h. Addresses refer to
// the original Driv3r.exe.

#include "story.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

// Entry types of the story table.
enum StepType
{
    T_MISSION  = 0,  // value = mission id
    T_RECAP    = 1,  // time = stop time in FMV\RECAP.XMV (seconds)
    T_CITY     = 2,  // value 3 (before Nice) / 5 (before Istanbul): 5..6 opens Istanbul (+0x24)
    T_PROGRESS = 3,  // value = furthest mission (+0x20) from here on
    T_VIDEO    = 4,  // path of a video
    T_END      = 5,
};

struct StoryStep { int type; int value; float time; const char* path; };

// Table 0x7a5878: 8-byte entries {type, value}; the recap times are floats
// and the video entries pointers in the original.
const StoryStep kStory[] = {
    { T_MISSION, 1 },    { T_RECAP, 0, 17.72f },  { T_PROGRESS, 2 },
    { T_MISSION, 2 },    { T_RECAP, 0, 24.88f },  { T_PROGRESS, 3 },
    { T_MISSION, 3 },    { T_RECAP, 0, 44.0f },   { T_PROGRESS, 4 },
    { T_MISSION, 4 },    { T_RECAP, 0, 44.0f },   { T_PROGRESS, 5 },
    { T_MISSION, 5 },    { T_RECAP, 0, 51.52f },  { T_PROGRESS, 6 },
    { T_MISSION, 6 },    { T_RECAP, 0, 51.52f },  { T_PROGRESS, 7 },
    { T_MISSION, 7 },    { T_RECAP, 0, 69.92f },  { T_PROGRESS, 8 },
    { T_MISSION, 8 },    { T_RECAP, 0, 77.92f },  { T_PROGRESS, 9 },
    { T_MISSION, 9 },    { T_RECAP, 0, 85.32f },  { T_PROGRESS, 10 },
    { T_MISSION, 10 },   { T_RECAP, 0, 91.52f },  { T_PROGRESS, 11 },
    { T_CITY, 3 },
    { T_MISSION, 11 },   { T_RECAP, 0, 104.4f },  { T_PROGRESS, 13 },
    { T_MISSION, 13 },   { T_RECAP, 0, 120.2f },  { T_PROGRESS, 14 },
    { T_MISSION, 14 },   { T_RECAP, 0, 132.2f },  { T_PROGRESS, 15 },
    { T_MISSION, 15 },   { T_RECAP, 0, 145.2f },  { T_PROGRESS, 16 },
    { T_MISSION, 16 },   { T_RECAP, 0, 150.2f },  { T_PROGRESS, 17 },
    { T_MISSION, 17 },   { T_RECAP, 0, 163.0f },  { T_PROGRESS, 18 },
    { T_MISSION, 18 },   { T_RECAP, 0, 168.52f }, { T_PROGRESS, 19 },
    { T_MISSION, 19 },   { T_RECAP, 0, 180.32f }, { T_PROGRESS, 21 },
    { T_MISSION, 21 },   { T_RECAP, 0, 198.92f }, { T_PROGRESS, 22 },
    { T_CITY, 5 },
    { T_MISSION, 22 },   { T_RECAP, 0, 204.96f }, { T_PROGRESS, 24 },
    { T_MISSION, 24 },   { T_RECAP, 0, 214.2f },  { T_PROGRESS, 25 },
    { T_MISSION, 25 },   { T_RECAP, 0, 224.8f },  { T_PROGRESS, 27 },
    { T_MISSION, 27 },   { T_RECAP, 0, 233.4f },  { T_PROGRESS, 28 },
    { T_MISSION, 28 },   { T_RECAP, 0, 259.48f }, { T_PROGRESS, 30 },
    { T_MISSION, 30 },   { T_RECAP, 0, 288.76f }, { T_PROGRESS, 31 },
    { T_MISSION, 31 },   { T_MISSION, 0xb2 },     { T_PROGRESS, 0x20 },
    { T_VIDEO, 0, 0, "FMV\\SC31.XMV" },
    { T_VIDEO, 0, 0, "FMV\\CREDITS.XMV" },
    { T_END },
};
const int kStoryCount = int(sizeof(kStory) / sizeof(kStory[0]));

}  // namespace

void StorySequencer::Logf(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    host_.Log(buf);
}

int StorySequencer::CurrentMission() const
{
    return kStory[pos_].type == T_MISSION ? kStory[pos_].value : 0;
}

void StorySequencer::ClearBlock()
{
    memset(block_, 0, sizeof(block_));  // rep stos, 0x400 dwords (0x553aa1)
}

// 0x557d10: from the first entry (mission 1). The profile's progress is
// left as it is.
void StorySequencer::NewGame()
{
    Logf("Story: new game");
    pos_ = 0;
    Step();
}

// 0x557d30: from the entry after the last completed mission (+0x1c). The
// end entry starts again from the top; the entry of mission 178 goes back
// one, to mission 31.
void StorySequencer::ContinueGame()
{
    int32_t at = host_.ProfileInt(0x1c);
    // The original does not check the range; a bad profile reads past the table.
    pos_ = at >= 0 && at < kStoryCount ? at : 0;
    if (kStory[pos_].type == T_END) pos_ = 0;
    if (kStory[pos_].type == T_MISSION && kStory[pos_].value == 0xb2) --pos_;
    Logf("Story: continue at entry %d", pos_);
    Step();
}

// 0x557d70: a mission found in the table starts at the entry after the
// mission before it (so a recap stop plays first); any other id (Take A
// Ride / Driving Games levels) goes straight to gameplay.
void StorySequencer::RunMission(int id)
{
    pos_ = 0;
    int i = 0;
    for (; kStory[i].type != T_END; ++i) {
        if (kStory[i].type != T_MISSION) continue;
        if (kStory[i].value == id) break;
        pos_ = i + 1;
    }
    if (kStory[i].type != T_END) {
        Logf("Story: run mission %d from entry %d", id, pos_);
        return Step();
    }
    Logf("Story: level 0x%x (not in the story)", id);
    setupId_ = id;
    host_.SetupMission(id);
    phase_ = MISSION;
    host_.GotoSequencer();
}

// 0x557f20: act on the current entry.
void StorySequencer::Step()
{
    const StoryStep& s = kStory[pos_];
    switch (s.type) {
    case T_RECAP:
        phase_ = RECAP;
        host_.GotoSequencer();
        return;
    case T_VIDEO:
        phase_ = VIDEO;
        host_.GotoSequencer();
        return;
    case T_CITY:
        CityStep(s.value);
        return Advance();
    case T_PROGRESS:
        RaiseProgress(s.value);
        return Advance();
    case T_END:
        Logf("Story: end");
        host_.GotoFrontEnd();
        return;
    default:  // T_MISSION
        phase_ = MISSION;
        host_.FreeSlots(s.value);  // 0x558080: only for missions 5, 11, 15, 17, 22, 28
        setupId_ = s.value;
        host_.SetupMission(s.value);
        host_.GotoSequencer();
        return;
    }
}

// 0x558000: to the next mission, video or end. Progress and city entries on
// the way are applied; recap stops are passed over (they only play when
// Continue or runmission= starts on one).
void StorySequencer::Advance()
{
    for (++pos_;; ++pos_) {
        int t = kStory[pos_].type;
        if (t == T_MISSION || t == T_END || t == T_VIDEO) break;
        if (t == T_PROGRESS) RaiseProgress(kStory[pos_].value);
        else if (t == T_CITY) CityStep(kStory[pos_].value);
    }
    Step();
}

// 0x557f8d / 0x55801d: furthest mission +0x20 = max(+0x20, value), unsigned.
void StorySequencer::RaiseProgress(int value)
{
    if (host_.ProgressLocked()) return;
    if (uint32_t(value) > uint32_t(host_.ProfileInt(0x20))) {
        host_.SetProfileInt(0x20, value);
        Logf("Story: furthest mission %d", value);
    }
}

// 0x557f65 / 0x55803b: value 5 or 6 opens Istanbul (+0x24 = 1).
void StorySequencer::CityStep(int value)
{
    if (host_.ProgressLocked()) return;
    if (value >= 5 && value <= 6) {
        host_.SetProfileByte(0x24, 1);
        Logf("Story: Istanbul open");
    }
}

// 0x557c20 (jump table 0x557cf8).
void StorySequencer::Update()
{
    switch (phase_) {
    case MISSION:
        Logf("Story: mission %d -> gameplay", setupId_);
        host_.StartGameplay(block_, sizeof(block_));
        return;
    case RECAP:
        host_.PlayVideo("FMV\\RECAP.XMV", kStory[pos_].time);
        return;
    case VIDEO:
        host_.PlayVideo(kStory[pos_].path, -1.0f);
        return;
    case RESTART:
    case QUIT:
        host_.GotoFrontEnd();
        return;
    case FINISHED:
        host_.MissionFinished();
        return Advance();
    }
}

// 0x557f00: the player removes the listener, then the story goes on.
void StorySequencer::VideoDone()
{
    Advance();
}

// 0x557de0 (message listener at +0xc).
void StorySequencer::OnMessage(const char* msg)
{
    Logf("Story: message %s", msg);
    if (_stricmp(msg, "complete") == 0) {
        if (host_.MissionPassed()) {
            // +0x1c = index of the entry after the mission, if higher.
            int next = pos_ + 1;
            if (!host_.ProgressLocked()) {
                if (uint32_t(host_.ProfileInt(0x1c)) < uint32_t(next)) {
                    host_.SetProfileInt(0x1c, next);
                    Logf("Story: position %d", next);
                }
                host_.CopyProfileBlock();
            } else {
                Logf("Story: progress locked (+0x5), nothing stored");
            }
            // Progress and city entries up to the next mission.
            for (int i = next; kStory[i].type != T_MISSION && kStory[i].type != T_END; ++i) {
                if (kStory[i].type == T_PROGRESS) RaiseProgress(kStory[i].value);
                else if (kStory[i].type == T_CITY) CityStep(kStory[i].value);
            }
        }
        phase_ = FINISHED;
    } else if (_stricmp(msg, "fail") == 0 || _stricmp(msg, "frontend") == 0) {
        phase_ = QUIT;
    } else if (_stricmp(msg, "restart") == 0) {
        phase_ = RESTART;
    }
}
