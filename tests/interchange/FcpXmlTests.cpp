// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Genesis-0 contributors
//
// The bounded XML reader and the FCPXML importer: the parse tree, malformed
// input, a flat sequence import with gap, and entity decoding on the way in.

#include <string>
#include <string_view>

#include "core/ClipTimeline.h"
#include "core/XmlRead.h"
#include "interchange/FcpXml.h"
#include "project/Project.h"
#include "project/model/Clip.h"
#include "project/model/Media.h"
#include "project/model/Timeline.h"
#include "project/model/VideoSettings.h"
#include "../support/Checks.h"
#include "Fixtures.h"

namespace {

using genesis::test::check;

namespace interchange = genesis::interchange;
namespace project = genesis::project;

using genesis::core::Rational;
using genesis::test::interchange::fixture;
using genesis::test::interchange::seconds;

// One picture on T1 and one sound on T2, both kinds of media in the bin.
project::Project picture_and_sound()
{
    project::Project edited;
    edited.timelines[0].name = "Cut";
    edited.timelines[0].video.width = 1920;
    edited.timelines[0].video.height = 1080;
    edited.timelines[0].video.rate_num = 30;
    edited.timelines[0].video.rate_den = 1;

    project::MediaItem picture;
    picture.id = "m1";
    picture.path = "/media/a.mp4";
    picture.name = "a.mp4";
    picture.kind = project::MediaKind::Video;
    edited.media.push_back(picture);

    project::MediaItem sound;
    sound.id = "m2";
    sound.path = "/media/b.wav";
    sound.name = "b.wav";
    sound.kind = project::MediaKind::Audio;
    edited.media.push_back(sound);

    project::Clip first;
    first.id = "c1";
    first.track_id = "T1";
    first.media_id = "m1";
    first.name = "One";
    first.kind = project::ClipKind::Video;
    first.start = seconds(0.0);
    first.duration = seconds(2.0);
    first.source_start = seconds(0.0);
    edited.timelines[0].clips.push_back(first);

    project::Clip audio;
    audio.id = "c2";
    audio.track_id = "T2";
    audio.media_id = "m2";
    audio.name = "Sound";
    audio.kind = project::ClipKind::Audio;
    audio.start = seconds(0.0);
    audio.duration = seconds(2.0);
    audio.source_start = seconds(1.0);
    edited.timelines[0].clips.push_back(audio);

    return edited;
}

} // namespace

void test_fcpxml_shape()
{
    const project::Project edited = fixture();
    const std::string text = interchange::to_fcpxml(edited);
    check(text.rfind("<?xml version=\"1.0\" encoding=\"UTF-8\"?>", 0) == 0,
          "the document starts with an XML declaration");
    check(text.find("<fcpxml version=\"1.9\">") != std::string::npos,
          "it declares the FCPXML version");
    check(text.find("<format id=\"r1\" frameDuration=\"1/30s\" width=\"1920\" height=\"1080\"/>")
                  != std::string::npos,
          "the format carries the frame duration and size");
    check(text.find("<asset id=\"r2\"") != std::string::npos
                  && text.find("src=\"file:///media/a.mp4\"") != std::string::npos,
          "a picture becomes an asset with a media reference");
    check(text.find("<asset id=\"r3\"") != std::string::npos
                  && text.find("src=\"file:///media/b.wav\"") != std::string::npos,
          "the sound becomes its own asset");
    check(text.find("<asset-clip ref=\"r3\" lane=\"1\"") != std::string::npos,
          "the sound rides a connected lane, not the picture spine");
    check(text.find("<video ref=\"r3\"") == std::string::npos,
          "the sound is never written as a picture element");
    check(text.find("<clip offset=\"0s\" duration=\"1s\" start=\"0s\" name=\"One\">")
                  != std::string::npos,
          "the first picture is a spine clip at zero");
    check(text.find("<clip offset=\"1s\" duration=\"2s\" start=\"5s\" name=\"Two\">")
                  != std::string::npos,
          "the second picture keeps its record offset and in-point");
    check(text.find("<video ref=\"r2\" offset=\"0s\" duration=\"2s\" start=\"5s\"/>")
                  != std::string::npos,
          "the clip points at its asset");
    check(text.find("<gap") == std::string::npos, "a contiguous cut needs no gap");

    // A hole in the spine becomes a gap so the cut keeps its timing.
    project::Project spaced = fixture();
    spaced.timelines[0].clips[1].start = seconds(4.0);
    const std::string with_gap = interchange::to_fcpxml(spaced);
    check(with_gap.find("<gap offset=\"1s\" duration=\"3s\" start=\"0s\"/>") != std::string::npos,
          "a hole between clips is written as a gap");

    // Names and paths are escaped, not pasted raw.
    project::Project awkward = fixture();
    awkward.timelines[0].clips[0].name = "A & B <take>";
    const std::string escaped = interchange::to_fcpxml(awkward);
    check(escaped.find("name=\"A &amp; B &lt;take&gt;\"") != std::string::npos,
          "element names are XML-escaped");
}

int main()
{
    // -- Parser: a small, well-formed document -------------------------------
    {
        const std::string doc = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                "<!DOCTYPE fcpxml>\n"
                                "<!-- a comment -->\n"
                                "<fcpxml version=\"1.9\">\n"
                                "  <resources>\n"
                                "    <format id=\"r1\" frameDuration=\"1001/30000s\"/>\n"
                                "    <asset id=\"r2\" name=\"a &amp; b\"/>\n"
                                "  </resources>\n"
                                "</fcpxml>\n";
        std::string error;
        const std::optional<genesis::core::XmlNode> tree = genesis::core::parse_xml(doc, error);
        check(tree.has_value(), std::string("well-formed document parses, error was: ") + error);
        if (!tree) {
            return genesis::test::summary();
        }
        const genesis::core::XmlNode *root = tree->child("fcpxml");
        check(root != nullptr, "root element is fcpxml");
        check(root != nullptr && root->child("resources") != nullptr,
              "fcpxml has a resources child");
        const genesis::core::XmlNode *resources =
                root != nullptr ? root->child("resources") : nullptr;
        check(resources != nullptr && resources->children.size() == 2,
              "resources has two children (comment and doctype not counted)");
        if (resources == nullptr || resources->children.size() != 2) {
            return genesis::test::summary();
        }
        const genesis::core::XmlNode &format = resources->children[0];
        const genesis::core::XmlNode &asset = resources->children[1];
        check(format.name == "format", "first resources child is format");
        const std::string *id = format.attribute("id");
        const std::string *frame = format.attribute("frameDuration");
        check(id != nullptr && *id == "r1", "format id attribute reads r1");
        check(frame != nullptr && *frame == "1001/30000s",
              "format frameDuration attribute reads verbatim");
        check(format.child("nope") == nullptr, "missing child name returns null");
        check(asset.name == "asset", "second resources child is asset");
        const std::string *name = asset.attribute("name");
        check(name != nullptr && *name == "a & b", "asset name decodes &amp; to &");
    }

    // -- Parser: CDATA is literal text, entities inside it are not expanded --
    {
        std::string error;
        const std::optional<genesis::core::XmlNode> tree =
                genesis::core::parse_xml("<a><b><![CDATA[x & <y>]]></b></a>", error);
        check(tree.has_value(), std::string("CDATA parses, error was: ") + error);
        if (tree) {
            const genesis::core::XmlNode *a = tree->child("a");
            const genesis::core::XmlNode *b = a != nullptr ? a->child("b") : nullptr;
            check(b != nullptr && b->text == "x & <y>", "CDATA content is taken literally");
        }
    }

    // -- Parser: malformed input is rejected with a reason -------------------
    {
        std::string error;
        const std::optional<genesis::core::XmlNode> unclosed =
                genesis::core::parse_xml("<fcpxml><resources>", error);
        check(!unclosed.has_value(), "unclosed tag returns nullopt");
        check(!error.empty(), "unclosed tag fills a non-empty error");
    }
    {
        std::string error;
        const std::optional<genesis::core::XmlNode> comment =
                genesis::core::parse_xml("<!-- never closed", error);
        check(!comment.has_value(), "unterminated comment returns nullopt");
        check(!error.empty(), "unterminated comment fills a non-empty error");
    }

    // -- Import: a flat sequence with gap and two clips ----------------------
    {
        const std::string xml =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<!DOCTYPE fcpxml>\n"
                "<fcpxml version=\"1.9\">\n"
                "  <resources>\n"
                "    <format id=\"r1\" frameDuration=\"1001/30000s\" width=\"1920\" "
                "height=\"1080\"/>\n"
                "    <asset id=\"r2\" name=\"a.mov\" start=\"0s\" duration=\"10s\" "
                "hasVideo=\"1\" hasAudio=\"1\">\n"
                "      <media-rep kind=\"original-media\" src=\"file:///media/a.mov\"/>\n"
                "    </asset>\n"
                "    <asset id=\"r3\" name=\"b.mov\" start=\"0s\" duration=\"10s\" "
                "hasVideo=\"1\" hasAudio=\"0\">\n"
                "      <media-rep kind=\"original-media\" src=\"media/b.mov\"/>\n"
                "    </asset>\n"
                "  </resources>\n"
                "  <library>\n"
                "    <event name=\"Genesis\">\n"
                "      <project name=\"My Cut\">\n"
                "        <sequence format=\"r1\" duration=\"40s\">\n"
                "          <spine>\n"
                "            <clip offset=\"0s\" duration=\"2s\" start=\"0s\" name=\"A\">\n"
                "              <video ref=\"r2\" offset=\"0s\" duration=\"2s\" start=\"0s\"/>\n"
                "            </clip>\n"
                "            <clip offset=\"2s\" duration=\"1s\" start=\"0s\" name=\"B\">\n"
                "              <video ref=\"r3\" offset=\"0s\" duration=\"1s\" start=\"0s\"/>\n"
                "            </clip>\n"
                "            <gap offset=\"3s\" duration=\"37s\" start=\"0s\"/>\n"
                "          </spine>\n"
                "        </sequence>\n"
                "      </project>\n"
                "    </event>\n"
                "  </library>\n"
                "</fcpxml>\n";
        project::VideoSettings video;
        video.rate_num = 25;
        video.rate_den = 1;
        video.width = 1280;
        video.height = 720;

        std::string error;
        const std::optional<project::Project> imported =
                interchange::from_fcpxml(xml, "/project/cut.fcpxml", video, error);
        check(imported.has_value(), std::string("sequence imports, error was: ") + error);
        if (!imported) {
            return genesis::test::summary();
        }

        const project::Project &project = *imported;
        check(project.timelines.size() == 1, "import produces one timeline");
        check(project.active_timeline_id == "TL1", "active timeline id is TL1");
        const project::Timeline &timeline = project.timelines.front();
        check(timeline.id == "TL1", "timeline id is TL1");
        check(timeline.name == "My Cut", "timeline name comes from project/@name");
        check(timeline.video.rate_num == 30000 && timeline.video.rate_den == 1001,
              "frameDuration 1001/30000s reads as 30000/1001");
        check(timeline.video.width == 1920 && timeline.video.height == 1080,
              "width and height come from the format resource");

        check(project.media.size() == 2, "two media items are imported");
        check(project.media[0].path == "/media/a.mov", "file:// URL resolves to an absolute path");
        check(project.media[1].path == "/project/media/b.mov",
              "relative src resolves against the source path");

        check(timeline.clips.size() == 2, "two clips survive (gap emits nothing)");
        if (timeline.clips.size() != 2) {
            return genesis::test::summary();
        }
        const project::Clip &first = timeline.clips[0];
        const project::Clip &second = timeline.clips[1];

        check(first.start == Rational::ZERO, "first clip starts at offset 0s");
        check(first.duration == Rational(2, 1), "first clip lasts 2s");
        check(first.source_start == Rational::ZERO, "first clip in-point is 0s");
        check(first.media_id == project.media[0].id, "first clip binds to the absolute asset");

        check(second.start == Rational(2, 1), "second clip starts at its own offset 2s");
        check(second.duration == Rational(1, 1), "second clip lasts 1s");
        check(second.source_start == Rational::ZERO, "second clip in-point is 0s");
        check(second.media_id == project.media[1].id, "second clip binds to the relative asset");
    }

    // -- Import: an unknown asset reference is dropped by settle ------------
    {
        const std::string xml =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<fcpxml version=\"1.9\">\n"
                "  <resources>\n"
                "    <format id=\"r1\" frameDuration=\"1/25s\"/>\n"
                "    <asset id=\"r2\" name=\"a.mov\" hasVideo=\"1\">\n"
                "      <media-rep kind=\"original-media\" src=\"file:///media/a.mov\"/>\n"
                "    </asset>\n"
                "  </resources>\n"
                "  <library>\n"
                "    <event name=\"Genesis\">\n"
                "      <project name=\"Cut\">\n"
                "        <sequence format=\"r1\">\n"
                "          <spine>\n"
                "            <clip offset=\"0s\" duration=\"2s\" start=\"0s\" name=\"A\">\n"
                "              <video ref=\"r2\"/>\n"
                "            </clip>\n"
                "            <clip offset=\"2s\" duration=\"1s\" start=\"0s\" name=\"Ghost\">\n"
                "              <video ref=\"r99\"/>\n"
                "            </clip>\n"
                "          </spine>\n"
                "        </sequence>\n"
                "      </project>\n"
                "    </event>\n"
                "  </library>\n"
                "</fcpxml>\n";
        project::VideoSettings video;
        std::string error;
        const std::optional<project::Project> imported =
                interchange::from_fcpxml(xml, "/project/cut.fcpxml", video, error);
        check(imported.has_value(), std::string("unknown ref still imports, error was: ") + error);
        if (!imported) {
            return genesis::test::summary();
        }
        // The importer keeps the clip with an empty media_id; the settle pass
        // (`settle_clip`) then drops it because its media names nothing, so
        // exactly one clip survives.
        check(imported->timelines.front().clips.size() == 1,
              "the clip whose ref names nothing is dropped by settle");
    }

    // -- Escaping: entity names round-trip through the importer --------------
    {
        const std::string xml =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<fcpxml version=\"1.9\">\n"
                "  <resources>\n"
                "    <format id=\"r1\" frameDuration=\"1/25s\"/>\n"
                "    <asset id=\"r2\" name=\"S &amp; S &lt; Lit\" hasVideo=\"1\">\n"
                "      <media-rep kind=\"original-media\" src=\"file:///media/a.mov\"/>\n"
                "    </asset>\n"
                "  </resources>\n"
                "  <library>\n"
                "    <event name=\"Genesis\">\n"
                "      <project name=\"Cut\">\n"
                "        <sequence format=\"r1\">\n"
                "          <spine>\n"
                "            <clip offset=\"0s\" duration=\"2s\" start=\"0s\" "
                "name=\"A &amp; B &lt; C\">\n"
                "              <video ref=\"r2\"/>\n"
                "            </clip>\n"
                "          </spine>\n"
                "        </sequence>\n"
                "      </project>\n"
                "    </event>\n"
                "  </library>\n"
                "</fcpxml>\n";
        project::VideoSettings video;
        std::string error;
        const std::optional<project::Project> imported =
                interchange::from_fcpxml(xml, "/project/cut.fcpxml", video, error);
        check(imported.has_value(), std::string("escaping document imports, error was: ") + error);
        if (!imported) {
            return genesis::test::summary();
        }
        check(!imported->media.empty() && imported->media[0].name == "S & S < Lit",
              "asset name decodes &amp; and &lt;");
        const project::Timeline &timeline = imported->timelines.front();
        check(!timeline.clips.empty() && timeline.clips.front().name == "A & B < C",
              "clip name decodes &amp; and &lt;");
    }

    // -- Import: a real editor's spine uses <asset-clip>, not <clip> ---------
    {
        const std::string xml =
                "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                "<fcpxml version=\"1.9\">\n"
                "  <resources>\n"
                "    <format id=\"r1\" frameDuration=\"1/25s\" width=\"1920\" height=\"1080\"/>\n"
                "    <asset id=\"r2\" name=\"shot.mov\" hasVideo=\"1\">\n"
                "      <media-rep kind=\"original-media\" src=\"file:///media/shot.mov\"/>\n"
                "    </asset>\n"
                "  </resources>\n"
                "  <library>\n"
                "    <event name=\"Genesis\">\n"
                "      <project name=\"Real Cut\">\n"
                "        <sequence format=\"r1\">\n"
                "          <spine>\n"
                "            <asset-clip ref=\"r2\" offset=\"0s\" duration=\"2s\" start=\"1s\" "
                "name=\"Shot\"/>\n"
                "          </spine>\n"
                "        </sequence>\n"
                "      </project>\n"
                "    </event>\n"
                "  </library>\n"
                "</fcpxml>\n";
        project::VideoSettings video;
        std::string error;
        const std::optional<project::Project> imported =
                interchange::from_fcpxml(xml, "/project/cut.fcpxml", video, error);
        check(imported.has_value(),
              std::string("an <asset-clip> spine imports, error was: ") + error);
        if (!imported) {
            return genesis::test::summary();
        }
        const project::Timeline &timeline = imported->timelines.front();
        check(timeline.clips.size() == 1, "an <asset-clip> becomes a clip");
        if (timeline.clips.size() != 1) {
            return genesis::test::summary();
        }
        check(timeline.clips[0].start == Rational::ZERO, "its offset is the record position");
        check(timeline.clips[0].duration == Rational(2, 1), "its duration carries over");
        check(timeline.clips[0].source_start == Rational(1, 1), "its start is the in-point");
        check(timeline.clips[0].media_id == imported->media.front().id, "its ref binds the asset");
    }

    // -- Import: a spine that names nothing recognisable fails loudly -------
    {
        const std::string xml = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                "<fcpxml version=\"1.9\">\n"
                                "  <resources>\n"
                                "    <format id=\"r1\" frameDuration=\"1/25s\"/>\n"
                                "  </resources>\n"
                                "  <library>\n"
                                "    <event name=\"Genesis\">\n"
                                "      <project name=\"Cut\">\n"
                                "        <sequence format=\"r1\">\n"
                                "          <spine>\n"
                                "            <asset-clip ref=\"r99\" offset=\"0s\" duration=\"2s\" "
                                "start=\"0s\"/>\n"
                                "          </spine>\n"
                                "        </sequence>\n"
                                "      </project>\n"
                                "    </event>\n"
                                "  </library>\n"
                                "</fcpxml>\n";
        project::VideoSettings video;
        std::string error;
        const std::optional<project::Project> imported =
                interchange::from_fcpxml(xml, "/project/cut.fcpxml", video, error);
        check(!imported.has_value(), "a spine whose clips all reference unknown media is refused");
        check(!error.empty(), "and the refusal explains itself");
    }

    // -- Export: a picture and its sound travel as a spine clip + connected
    //    audio clip -----------------------------------------------------------
    {
        const std::string text = interchange::to_fcpxml(picture_and_sound());

        // The picture and the audio each declare an asset; the picture keeps
        // `r2` and the audio follows as `r3`, so a later audio edit cannot
        // shift the picture's id.
        check(text.find("<asset id=\"r2\" name=\"a.mp4\"") != std::string::npos,
              "the picture declares its asset first");
        check(text.find("<asset id=\"r3\" name=\"b.wav\"") != std::string::npos,
              "the audio declares its asset after the picture's");
        check(text.find("<asset id=\"r4\"") == std::string::npos, "no idle asset is declared");

        // The picture spine is unchanged: its gap handling and clip shape are
        // the video-only export's.
        check(text.find("<clip offset=\"0s\" duration=\"2s\" start=\"0s\" name=\"One\">")
                      != std::string::npos,
              "the picture spine clip is untouched");
        check(text.find("<video ref=\"r2\" offset=\"0s\" duration=\"2s\" start=\"0s\"/>")
                      != std::string::npos,
              "the picture spine still points at its asset");

        // The sound is a connected clip carrying its record offset, source
        // in-point and name, and it keeps its parallel timing via `lane="1"`.
        check(text.find("<asset-clip ref=\"r3\" lane=\"1\" offset=\"0s\" duration=\"2s\" "
                        "start=\"1s\" name=\"Sound\"/>")
                      != std::string::npos,
              "the audio becomes a connected clip with its timecodes");
        {
            std::size_t lane_ones = 0;
            for (std::size_t at = 0; (at = text.find("lane=\"1\"", at)) != std::string::npos;
                 ++at) {
                ++lane_ones;
            }
            check(lane_ones == 1, "exactly one connected clip is emitted");
        }
    }

    test_fcpxml_shape();
    return genesis::test::summary();
}
