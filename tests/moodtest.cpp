// Checks mood.cpp on the shipped moods: reads every Moods\MoodNN.txt with
// Mood::Load and prints the values the renderer uses; then, for every Take
// A Ride city / time / weather, RideMoodNumber and that mood's TIME_OF_DAY
// and sky, and counts the entries whose time differs from the time asked
// for (the files' WEATHER lines are not used by the number).
// Build: tests\build_moodtest.bat. Usage: moodtest <game dir>
#include <cstdio>
#include <cstring>
#include <string>

#include "../mood.h"

static std::string Field(const std::string& path, const char* key)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    char line[256];
    std::string v;
    while (fgets(line, sizeof(line), f))
        if (strncmp(line, key, strlen(key)) == 0) {
            const char* p = line + strlen(key);
            while (*p == ':' || *p == ' ' || *p == '\t') ++p;
            v = p;
            while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();
            break;
        }
    fclose(f);
    return v;
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: moodtest <game dir>\n"); return 1; }
    std::string dir = argv[1];
    int read = 0;
    for (int n = 1; n < 100; ++n) {
        char name[32];
        snprintf(name, sizeof(name), "Moods\\Mood%02d.txt", n);
        Mood m;
        if (!m.Load(dir + "\\" + name)) continue;
        ++read;
        float fog[4];
        m.FogParams(fog);
        printf("%02d %-34s amb %.3f %.3f %.3f dir %.3f %.3f %.3f vec %.3f %.3f %.3f fog %g %g %g %g colour %08lx"
               " sky row0 %.3f %.3f %.3f\n",
               n, m.skydome.c_str(), m.ambient[0], m.ambient[1], m.ambient[2], m.directional[0], m.directional[1],
               m.directional[2], m.direction[0], m.direction[1], m.direction[2], fog[0], fog[1], fog[2], fog[3],
               m.FogColour(), m.skyMatrix[0], m.skyMatrix[1], m.skyMatrix[2]);
    }
    printf("%d moods read\n", read);
    static const char* kCity[] = { "Miami", "Nice", "Istanbul" };
    static const char* kTime[] = { "DAWN", "DAY", "DUSK", "NIGHT" };
    static const char* kWeather[] = { "dry", "overcast", "wet" };
    int mismatches = 0;
    for (int c = 0; c < 3; ++c)
        for (int w = 0; w < 3; ++w)
            for (int t = 0; t < 4; ++t) {
                int n = RideMoodNumber(c, t, w);
                char name[32];
                snprintf(name, sizeof(name), "Moods\\Mood%02d.txt", n);
                std::string path = dir + "\\" + name, tod = Field(path, "TIME_OF_DAY"), sky = Field(path, "SKYDOME_NAME");
                bool ok = tod == kTime[t];
                if (!ok) ++mismatches;
                printf("%-8s %-8s %-5s -> %02d %-5s %s%s\n", kCity[c], kWeather[w], kTime[t], n, tod.c_str(), sky.c_str(),
                       ok ? "" : "  <- time differs");
            }
    printf("%d of 36 ride moods with another TIME_OF_DAY\n", mismatches);
    return 0;
}
