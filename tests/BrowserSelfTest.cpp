#include "BrowserSelfTest.h"
#include "Harness.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "audio/AudioFile.h"
#include "audio/SampleSet.h"
#include "daw/Archive.h"
#include "daw/Json.h"
#include "daw/Library.h"
#include "daw/Online.h"
#include "env/Environment.h"

// stb's zlib compressor, to make deflated zip entries
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#include <stb_image_write.h>

namespace tests {

namespace {

namespace fs = std::filesystem;

void writeText(const fs::path &path, const std::string &text)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << text;
}

std::string readText(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}

// A short tone as a WAV file
void writeTone(const fs::path &path, double hz, float level, uint32_t frames = 4800)
{
    std::vector<float> samples(frames);
    for (uint32_t i = 0; i < frames; ++i)
        samples[i] = level * static_cast<float>(std::sin(2.0 * 3.14159265358979 * hz * i / 48000.0));
    fs::create_directories(path.parent_path());
    std::string error;
    audio::writeWavFile(path.string(), samples.data(), frames, 1, 48000.0, error);
}

// A zip file, built by hand: entries stored or deflated
struct ZipEntry {
    std::string name;
    std::string data;
    bool deflate = false;
    bool badCrc = false;
};

void put16(std::string &s, uint32_t v)
{
    s += static_cast<char>(v & 0xFF);
    s += static_cast<char>((v >> 8) & 0xFF);
}
void put32(std::string &s, uint32_t v)
{
    put16(s, v & 0xFFFF);
    put16(s, v >> 16);
}

std::string makeZip(const std::vector<ZipEntry> &entries)
{
    std::string out, dir;
    for (const ZipEntry &e : entries) {
        std::string packed = e.data;
        uint16_t method = 0;
        if (e.deflate) {
            int length = 0;
            unsigned char *z = stbi_zlib_compress(reinterpret_cast<unsigned char *>(const_cast<char *>(e.data.data())),
                                                  static_cast<int>(e.data.size()), &length, 8);
            packed.assign(reinterpret_cast<char *>(z) + 2, static_cast<size_t>(length - 6)); // raw deflate
            free(z);
            method = 8;
        }
        uint32_t crc = daw::crc32(e.data.data(), e.data.size());
        if (e.badCrc)
            crc ^= 1;
        const uint32_t offset = static_cast<uint32_t>(out.size());
        put32(out, 0x04034b50);
        put16(out, 20);
        put16(out, 0);
        put16(out, method);
        put32(out, 0);
        put32(out, crc);
        put32(out, static_cast<uint32_t>(packed.size()));
        put32(out, static_cast<uint32_t>(e.data.size()));
        put16(out, static_cast<uint32_t>(e.name.size()));
        put16(out, 0);
        out += e.name;
        out += packed;

        put32(dir, 0x02014b50);
        put16(dir, 20);
        put16(dir, 20);
        put16(dir, 0);
        put16(dir, method);
        put32(dir, 0);
        put32(dir, crc);
        put32(dir, static_cast<uint32_t>(packed.size()));
        put32(dir, static_cast<uint32_t>(e.data.size()));
        put16(dir, static_cast<uint32_t>(e.name.size()));
        put16(dir, 0);
        put16(dir, 0);
        put16(dir, 0);
        put16(dir, 0);
        put32(dir, 0);
        put32(dir, offset);
        dir += e.name;
    }
    const uint32_t dirOffset = static_cast<uint32_t>(out.size());
    out += dir;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<uint32_t>(entries.size()));
    put16(out, static_cast<uint32_t>(entries.size()));
    put32(out, static_cast<uint32_t>(dir.size()));
    put32(out, dirOffset);
    put16(out, 0);
    return out;
}

}

int runBrowserSelfTest()
{
    Checker c;
    const fs::path temp = fs::temp_directory_path() / "vibin-browser-selftest";
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp);

    // JSON
    {
        daw::toml::Value v;
        std::string error;
        const bool ok = daw::json::parse(
            R"({"name": "A \"q\" \u00e9 \ud83c\udfb8", "n": 42, "f": -1.5e2, "t": true, "z": null,
                "list": [1, 2, {"k": "v"}], "empty": {}, "arr": []})",
            v, error);
        c.check(ok && v.get("name", "") == "A \"q\" \xC3\xA9 \xF0\x9F\x8E\xB8", "JSON reads strings and escapes", error);
        c.check(ok && v.get("n", 0) == 42 && v.at("n")->type() == daw::toml::Type::Integer &&
                    std::fabs(v.get("f", 0.0) + 150.0) < 1e-9 && v.get("t", false),
                "JSON reads numbers and booleans");
        c.check(ok && v.at("z") && v.get("z", "fallback") == "fallback", "JSON null falls back to defaults");
        const daw::toml::Array *list = v.arrayAt("list");
        c.check(list && list->size() == 3 && (*list)[2].get("k", "") == "v", "JSON reads nested arrays and objects");
        bool refused = true;
        for (const char *bad : {"{\"a\": }", "[1, 2", "{\"a\" 1}", "\"open", "tru", "{} extra"}) {
            daw::toml::Value w;
            std::string why;
            refused = refused && !daw::json::parse(bad, w, why) && !why.empty();
        }
        c.check(refused, "JSON refuses broken text with a reason");
    }

    // Names, licences, categories
    c.check(daw::prettyRepoName("karoryfer.272-merry-orks") == "272 Merry Orks" &&
                daw::prettyRepoName("SalamanderGrandPiano") == "Salamander Grand Piano" &&
                daw::prettyRepoName("DrumGizmo.DRSKit") == "DRS Kit" &&
                daw::prettyRepoName("dsmolken.double-bass") == "Double Bass",
            "Repository names read as names",
            daw::prettyRepoName("karoryfer.272-merry-orks") + ", " + daw::prettyRepoName("DrumGizmo.DRSKit"));
    c.check(daw::licenseName("http://creativecommons.org/publicdomain/zero/1.0/") == "CC0" &&
                daw::licenseName("https://creativecommons.org/licenses/by/4.0/") == "CC BY 4.0" &&
                daw::licenseName("http://creativecommons.org/licenses/by-nc/3.0/") == "CC BY NC 3.0",
            "Licence addresses read as names", daw::licenseName("https://creativecommons.org/licenses/by/4.0/"));
    c.check(daw::guessCategory("Spanish classical guitar") == "Guitars" && daw::guessCategory("Bass guitar") == "Bass" &&
                daw::guessCategory("Salamander Grand Piano") == "Keys" && daw::guessCategory("DRS Kit") == "Drums" &&
                daw::guessCategory("Dim Cabasa") == "Percussion" && daw::guessCategory("Cello") == "Strings" &&
                daw::guessCategory("Drum loop 120") == "Loops" && daw::guessCategory("Qwerty") == "Other",
            "Categories are guessed from names");
    c.check(daw::freesoundQuery("", "Drums") == "drum" && daw::freesoundQuery("kick", "") == "kick" &&
                daw::freesoundQuery("", "") == "one shot" && daw::freesoundQuery("808", "Bass") == "808 bass",
            "Freesound queries follow the search and category", daw::freesoundQuery("808", "Bass"));

    // Listings from canned answers
    {
        daw::SourceConfig gh;
        gh.name = "GH";
        gh.type = "github";
        gh.org = "sfzinstruments";
        gh.exclude = {"mappings"};
        const std::string json = R"([
            {"name": "karoryfer.emilyguitar", "description": "Karoryfer Samples open source instrument Emilyguitar",
             "size": 101022, "default_branch": "master", "html_url": "https://github.com/sfzinstruments/karoryfer.emilyguitar",
             "license": {"spdx_id": "CC0-1.0", "name": "Creative Commons Zero v1.0 Universal"}, "archived": false, "fork": false},
            {"name": "mappings", "size": 1383, "license": null},
            {"name": "tiny", "size": 3},
            {"name": "old", "size": 5000, "archived": true},
            {"name": "SalamanderGrandPiano", "description": "Salamander Grand Piano v3", "size": 730959,
             "default_branch": "main", "license": {"spdx_id": "NOASSERTION", "name": "Other"}}
        ])";
        std::string error;
        const auto items = daw::parseGithubRepos(json, gh, error);
        const bool two = items.size() == 2;
        c.check(two && error.empty(), "GitHub listing keeps real instruments", std::to_string(items.size()) + " items " + error);
        if (two) {
            const daw::OnlineItem &emily = items[0].name == "Emilyguitar" ? items[0] : items[1];
            const daw::OnlineItem &piano = items[0].name == "Emilyguitar" ? items[1] : items[0];
            c.check(emily.url == "https://codeload.github.com/sfzinstruments/karoryfer.emilyguitar/zip/refs/heads/master" &&
                        emily.category == "Guitars" && emily.license == "CC0-1.0" && emily.author == "karoryfer" &&
                        emily.bytes == 101022ull * 1024 && emily.kind == daw::AssetKind::Instrument,
                    "GitHub items download as zips", emily.url);
            c.check(piano.url.find("/zip/refs/heads/main") != std::string::npos && piano.category == "Keys" &&
                        piano.license == "See its licence file",
                    "GitHub items follow the branch and licence", piano.license);
        }
        std::string why;
        daw::parseGithubRepos(R"({"message": "API rate limit exceeded"})", gh, why);
        c.check(why == "API rate limit exceeded", "GitHub's refusal is shown", why);

        daw::SourceConfig fsSource;
        fsSource.name = "Freesound";
        fsSource.type = "freesound";
        const std::string fsJson = R"({"count": 2, "results": [
            {"id": 123, "name": "Kick 01.wav", "tags": ["kick", "drum"], "license": "http://creativecommons.org/publicdomain/zero/1.0/",
             "username": "someone", "duration": 0.42, "previews": {"preview-hq-mp3": "https://cdn.example/123-hq.mp3"},
             "url": "https://freesound.org/people/someone/sounds/123/"},
            {"id": 7, "name": "No preview", "previews": {}}
        ]})";
        error.clear();
        const auto sounds = daw::parseFreesound(fsJson, fsSource, error);
        c.check(sounds.size() == 1 && sounds[0].name == "Kick 01" && sounds[0].fileName == "Kick 01.mp3" &&
                    sounds[0].category == "Drums" && sounds[0].license == "CC0" && sounds[0].kind == daw::AssetKind::Sample &&
                    sounds[0].url == "https://cdn.example/123-hq.mp3" && std::fabs(sounds[0].seconds - 0.42) < 1e-9,
                "Freesound results are samples with previews",
                sounds.empty() ? error : sounds[0].name + " / " + sounds[0].category);

        daw::SourceConfig cat;
        cat.name = "Mine";
        cat.type = "catalogue";
        const std::string catalogue = R"(
            [[items]]
            name = "Big drums"
            kind = "pack"
            url = "https://example.com/drums.zip"
            size = 1048576
            [[items]]
            name = "Snap"
            kind = "sample"
            url = "https://example.com/snap.flac?dl=1"
            category = "Percussion"
            [[items]]
            name = "No address"
        )";
        error.clear();
        const auto mine = daw::parseCatalogue(catalogue, cat, error);
        c.check(mine.size() == 2 && mine[0].kind == daw::AssetKind::Pack && mine[0].category == "Drums" &&
                    mine[0].bytes == 1048576 && mine[1].fileName == "Snap.flac" && mine[1].category == "Percussion",
                "Catalogues list packs and samples", std::to_string(mine.size()) + " items " + error);

        std::vector<daw::SourceConfig> sources;
        error.clear();
        const bool read = daw::readSources(R"(
            [[sources]]
            type = "github"
            org = "someorg"
            exclude = ["skip"]
            [[sources]]
            type = "catalogue"
            name = "Local"
            url = "C:/sounds/list.toml"
        )", sources, error);
        c.check(read && sources.size() == 2 && sources[0].name == "someorg" && sources[0].exclude.size() == 1 &&
                    sources[1].url == "C:/sounds/list.toml" && !sources[0].searchable(),
                "sources.toml adds sources", error);
        std::vector<daw::SourceConfig> broken;
        std::string brokenWhy;
        c.check(!daw::readSources("[[sources]]\ntype = \"ftp\"\n", broken, brokenWhy) && !brokenWhy.empty(),
                "Unknown source types are refused", brokenWhy);
        c.check(daw::defaultSources().size() == 2 && daw::defaultSources()[1].searchable(),
                "Default sources: GitHub and Freesound");
    }

    // Zip files
    {
        std::string big;
        for (int i = 0; i < 4000; ++i)
            big += "region " + std::to_string(i % 37) + " ";
        const fs::path zip = temp / "test.zip";
        writeText(zip, makeZip({{"repo-master/", "", false},
                                {"repo-master/readme.txt", "hello", false},
                                {"repo-master/Programs/inst.sfz", big, true},
                                {"repo-master/empty.txt", "", true}}));
        std::string error;
        daw::ExtractProgress progress;
        const bool ok = daw::extractArchive(zip.string(), (temp / "out").string(), error, &progress);
        c.check(ok && readText(temp / "out/repo-master/readme.txt") == "hello", "Zip: stored entries unpack", error);
        c.check(ok && readText(temp / "out/repo-master/Programs/inst.sfz") == big, "Zip: deflated entries unpack",
                std::to_string(fs::exists(temp / "out/repo-master/Programs/inst.sfz", ec) ? fs::file_size(temp / "out/repo-master/Programs/inst.sfz", ec) : 0) + " bytes");
        c.check(ok && progress.total == big.size() + 5 && progress.done == progress.total, "Zip: progress counts the bytes",
                std::to_string(progress.done.load()) + " of " + std::to_string(progress.total.load()));

        writeText(temp / "bad.zip", makeZip({{"x.txt", "some text here", true, true}}));
        error.clear();
        c.check(!daw::extractZip((temp / "bad.zip").string(), (temp / "bad").string(), error) &&
                    error.find("checksum") != std::string::npos,
                "Zip: damaged entries are refused", error);
        writeText(temp / "evil.zip", makeZip({{"../evil.txt", "boo", false}}));
        error.clear();
        c.check(!daw::extractZip((temp / "evil.zip").string(), (temp / "evil").string(), error) &&
                    !fs::exists(temp / "evil.txt", ec),
                "Zip: paths outside the folder are refused", error);
        writeText(temp / "notzip.zip", "this is not a zip file at all, just text that goes on for a while");
        error.clear();
        c.check(!daw::extractZip((temp / "notzip.zip").string(), (temp / "nz").string(), error) && !error.empty(),
                "Zip: other files are refused", error);
        c.check(daw::isArchive("a.ZIP") && daw::isArchive("b.tar.gz") && daw::isArchive("c.7z") && !daw::isArchive("d.wav"),
                "Archive types by extension");
    }

    // The SFZ loader
    {
        const fs::path inst = temp / "sfz";
        // Six velocity layers on one key range (thinned to four), a round robin, a release
        // trigger, two keyswitches, a loop and a tuning
        for (int layer = 0; layer < 6; ++layer)
            writeTone(inst / "samples" / ("v" + std::to_string(layer) + ".wav"), 220.0, 0.1f + 0.1f * layer);
        writeTone(inst / "samples/rr2.wav", 220.0, 0.9f);
        writeTone(inst / "samples/rel.wav", 220.0, 0.9f);
        writeTone(inst / "samples/sw2.wav", 330.0, 0.5f);
        writeTone(inst / "samples/loop.wav", 440.0, 0.5f, 9600);
        std::string layers;
        for (int layer = 0; layer < 6; ++layer)
            layers += "<region> sample=v" + std::to_string(layer) + ".wav lovel=" + std::to_string(layer * 21 + 1) +
                      " hivel=" + std::to_string(layer == 5 ? 127 : layer * 21 + 21) + "\n";
        writeText(inst / "layers.sfzh", "<group> lokey=40 hikey=50 pitch_keycenter=$ROOT seq_length=2 seq_position=1\n" + layers +
                                            "<group> lokey=40 hikey=50 seq_position=2\n<region> sample=rr2.wav\n"
                                            "<group> trigger=release\n<region> sample=rel.wav lokey=40 hikey=50\n");
        writeText(inst / "Main.sfz", "// A test instrument\n"
                                     "#define $ROOT 45\n"
                                     "/* a block\n comment */\n"
                                     "<control> default_path=samples/\n"
                                     "<global> sw_default=24 ampeg_release=0.5\n"
                                     "#include \"layers.sfzh\"\n"
                                     "<group> sw_last=24\n"
                                     "<region> sample=loop.wav key=60 loop_mode=loop_continuous loop_start=100 loop_end=4000 tune=-50\n"
                                     "<group> sw_last=25\n"
                                     "<region> sample=sw2.wav key=60\n");
        std::string error;
        auto set = audio::loadSfz((inst / "Main.sfz").string(), error);
        c.check(set != nullptr, "SFZ with #include and #define loads", error);
        if (set) {
            std::vector<const audio::SampleZone *> layered;
            const audio::SampleZone *looped = nullptr;
            bool stray = false;
            for (const audio::SampleZone &z : set->zones) {
                if (z.lowKey == 40 && z.highKey == 50)
                    layered.push_back(&z);
                if (z.lowKey == 60)
                    looped = &z;
                if (z.file.find("rr2") != std::string::npos || z.file.find("rel.wav") != std::string::npos ||
                    z.file.find("sw2") != std::string::npos)
                    stray = true;
            }
            c.check(set->zones.size() == 5 && !stray,
                    "Round robins, release triggers, other keyswitches left out",
                    std::to_string(set->zones.size()) + " zones");
            std::sort(layered.begin(), layered.end(),
                      [](const auto *a, const auto *b) { return a->lowVelocity < b->lowVelocity; });
            bool covers = layered.size() == static_cast<size_t>(audio::kMaxVelocityLayers) && layered.front()->lowVelocity == 1 &&
                          layered.back()->highVelocity == 127;
            for (size_t i = 1; covers && i < layered.size(); ++i)
                covers = layered[i]->lowVelocity == layered[i - 1]->highVelocity + 1;
            c.check(covers, "Six velocity layers thinned to four, covering 1-127",
                    std::to_string(layered.size()) + " layers");
            c.check(!layered.empty() && layered.front()->rootKey == 45, "#define values are substituted",
                    layered.empty() ? "" : "root " + std::to_string(layered.front()->rootKey));
            const audio::SampleZone *soft = set->zoneFor(45, 10);
            const audio::SampleZone *hard = set->zoneFor(45, 120);
            c.check(soft && hard && soft != hard && soft->lowVelocity == 1 && hard->highVelocity == 127,
                    "Velocity picks the layer");
            c.check(looped && looped->loopStart == 100 && looped->loopEnd == 4000 &&
                        std::fabs(looped->tuneCents + 50.0f) < 1e-3f && std::fabs(set->releaseSeconds - 0.5f) < 1e-4f,
                    "Loops, tuning and release are read",
                    looped ? std::to_string(looped->loopStart) + "-" + std::to_string(looped->loopEnd) : "no zone");
        }
        std::string display, why;
        std::vector<std::string> includes;
        const auto paths = audio::sfzSamplePaths((inst / "Main.sfz").string(), display, why, &includes);
        c.check(paths.size() == 10 && includes.size() == 1 && includes[0] == "layers.sfzh",
                "Sample paths include the included files'", std::to_string(paths.size()) + " samples");
        writeText(temp / "deep/Top/Programs/x.sfz", "<region> sample=../s.wav\n");
        c.check(audio::findSfz((temp / "deep").string()).find("x.sfz") != std::string::npos,
                "An SFZ two folders down is found");
    }

    // The library, in a temporary folder
    {
        daw::Settings settings;
        settings.libraryPath = (temp / "Library").string();
        const fs::path lib = temp / "Library";
        // An old-style instrument directly in Instruments, one in a category, one with its SFZ
        // in a subfolder, and samples with a pack
        writeTone(lib / "Instruments/Old guitar/E2.wav", 82.4, 0.5f);
        writeTone(lib / "Instruments/Keys/Little piano/C4.wav", 261.6, 0.5f);
        writeTone(lib / "Instruments/Strings/Cello repo/Programs/samples/C2.wav", 65.4, 0.5f);
        writeText(lib / "Instruments/Strings/Cello repo/Programs/cello.sfz", "<region> sample=samples/C2.wav key=36\n");
        writeTone(lib / "Samples/Drums/Kick.wav", 50.0, 0.8f);
        writeTone(lib / "Samples/Drums/Acoustic kit/Snare.wav", 200.0, 0.6f);
        writeTone(lib / "Samples/Loose clap.wav", 900.0, 0.6f);
        env::Environment environment;
        daw::InstrumentLibrary library(environment, settings);
        const auto *old = library.find("Old guitar");
        const auto *piano = library.find("Little piano");
        const auto *cello = library.find("Cello repo");
        const std::string celloFolder = cello ? cello->folder : std::string();
        c.check(old && old->category == "Guitars" && piano && piano->category == "Keys" && cello &&
                    cello->category == "Strings" && library.entries().size() == 3,
                "Instruments are found in category folders and the old layout",
                std::to_string(library.entries().size()) + " instruments");
        bool pack = false, loose = false;
        for (const daw::LibrarySample &s : library.samples()) {
            pack = pack || (s.name == "Snare" && s.pack == "Acoustic kit" && s.category == "Drums");
            loose = loose || (s.name == "Loose clap" && s.category == "Percussion");
        }
        c.check(library.samples().size() == 3 && pack && loose, "Samples are found with their categories and packs",
                std::to_string(library.samples().size()) + " samples");
        const uint64_t version = library.version();

        std::string error;
        writeTone(temp / "download/x-master/A3.wav", 220.0, 0.5f);
        const std::string installed = library.installInstrument((temp / "download/x-master").string(), "Little piano",
                                                                "Keys", error);
        c.check(!installed.empty() && library.find("Little piano 2") && library.version() > version,
                "An install with a taken name gets its own", fs::u8path(installed).filename().string() + " " + error);
        c.check(library.moveInstrument("Old guitar", "Bass", error) && library.find("Old guitar")->category == "Bass" &&
                    fs::exists(lib / "Instruments/Bass/Old guitar/E2.wav", ec),
                "Moving an instrument moves its folder", error);
        writeTone(temp / "outside/Hat.wav", 5000.0, 0.3f);
        const int copied = library.importSamples({(temp / "outside/Hat.wav").string(), (temp / "outside/none.txt").string()},
                                                 "Drums", error);
        c.check(copied == 1 && fs::exists(lib / "Samples/Drums/Hat.wav", ec) && library.samples().size() == 4,
                "Samples are copied into their category", std::to_string(copied) + " copied");
        const auto cats = library.categories();
        c.check(cats.front() == "Guitars" && std::find(cats.begin(), cats.end(), "Drums") != cats.end(),
                "Categories start with the defaults");
        error.clear();
        auto set = audio::loadInstrumentFolder(celloFolder, error);
        c.check(set && set->zones.size() == 1, "An instrument with its SFZ in a subfolder loads", error);
    }

    fs::remove_all(temp, ec);
    return c.finish("browser");
}

}
