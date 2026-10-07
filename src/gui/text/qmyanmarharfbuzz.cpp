#include "qmyanmarharfbuzz_p.h"
#include "qfontengine_p.h"
#include <hb.h>
#include <hb-ot.h>
#include <limits.h>
QT_BEGIN_NAMESPACE

bool qt_myanmar_harfbuzz_enabled()
{
    // Experimental until font fallback, layout and PDF mapping validation passes.
    return qgetenv("QT_MYANMAR_HARFBUZZ") == "1";
}

bool qt_myanmar_character(uint character)
{
    return hb_unicode_script(hb_unicode_funcs_get_default(), character) == HB_SCRIPT_MYANMAR;
}

bool qt_myanmar_continuation(uint character, uint previous)
{
    hb_unicode_general_category_t category = hb_unicode_general_category(hb_unicode_funcs_get_default(), character);
    return previous == 0x1039 || character == 0x200c || character == 0x200d
        || category == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK
        || category == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK
        || category == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK;
}

namespace {
void deleteTable(void *data) { delete static_cast<QByteArray *>(data); }
hb_blob_t *table(hb_face_t *, hb_tag_t tag, void *data)
{
    QByteArray *bytes = new QByteArray(static_cast<QFontEngine *>(data)->getSfntTable(tag));
    return hb_blob_create(bytes->constData(), bytes->size(), HB_MEMORY_MODE_READONLY, bytes, deleteTable);
}
struct Font {
    hb_face_t *face;
    hb_font_t *font;
    Font(QFontEngine *engine) {
        face = hb_face_create_for_tables(table, engine, 0);
        font = hb_font_create(face);
        hb_ot_font_set_funcs(font);
        const int scale = QFixed::fromReal(engine->fontDef.pixelSize).value();
        const int xscale = scale * qint64(engine->fontDef.stretch) / 100;
        hb_font_set_scale(font, xscale, scale);
    }
    ~Font() { hb_font_destroy(font); hb_face_destroy(face); }
private:
    Font(const Font &);
    Font &operator=(const Font &);
};
struct FontCache {
    QVector<uint> indices;
    QVector<Font *> fonts;
    ~FontCache() { for (int i = 0; i < fonts.size(); ++i) delete fonts[i]; }
    Font *get(uint index, QFontEngine *engine) {
        int at = indices.indexOf(index);
        if (at >= 0) return fonts[at];
        Font *font = new Font(engine); indices.append(index); fonts.append(font); return font;
    }
};
struct Buffer {
    hb_buffer_t *buffer;
    Buffer() : buffer(hb_buffer_create()) {}
    ~Buffer() { hb_buffer_destroy(buffer); }
};

bool ignored(uint c)
{
    return c == 0x200b || c == 0x200c || c == 0x200d || (c >= 0xfe00 && c <= 0xfe0f);
}
bool covers(hb_font_t *font, const QString &text)
{
    for (int i = 0; i < text.size(); ++i) {
        uint c = text.at(i).unicode();
        if (QChar::isHighSurrogate(c) && i + 1 < text.size() && text.at(i + 1).isLowSurrogate())
            { ushort high = c; ++i; c = QChar::surrogateToUcs4(high, text.at(i).unicode()); }
        hb_codepoint_t glyph;
        if (!ignored(c) && (!hb_font_get_nominal_glyph(font, c, &glyph) || !glyph)) return false;
    }
    return true;
}
bool shape(const QString &text, hb_font_t *font, bool kerning, Buffer *buffer)
{
    hb_buffer_set_cluster_level(buffer->buffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
    hb_buffer_add_utf16(buffer->buffer, reinterpret_cast<const uint16_t *>(text.utf16()), text.size(), 0, text.size());
    hb_buffer_set_script(buffer->buffer, HB_SCRIPT_MYANMAR);
    hb_buffer_set_direction(buffer->buffer, HB_DIRECTION_LTR);
    hb_buffer_set_language(buffer->buffer, hb_language_from_string("my", -1));
    hb_feature_t feature = { HB_TAG('k','e','r','n'), kerning ? 1u : 0u, 0, UINT_MAX };
    hb_shape(font, buffer->buffer, &feature, 1);
    return hb_buffer_allocation_successful(buffer->buffer);
}
bool append(hb_buffer_t *buffer, int from, int length, uint selected, QFontEngine *actual, QMyanmarShape *result)
{
        unsigned n;
        const hb_glyph_info_t *glyphs = hb_buffer_get_glyph_infos(buffer, &n);
        const hb_glyph_position_t *positions = hb_buffer_get_glyph_positions(buffer, &n);
        if (!n || result->glyphs.size() + n > USHRT_MAX || glyphs[0].cluster != 0) return false;
        int base = result->glyphs.size();
        for (unsigned i = 0; i < n; ++i) {
            if (glyphs[i].cluster >= unsigned(length) || (i && glyphs[i].cluster < glyphs[i-1].cluster)
                || glyphs[i].codepoint > 0xffffff || positions[i].y_offset == INT_MIN) return false;
            QMyanmarGlyph g;
            g.glyph = glyphs[i].codepoint | (selected << 24);
            g.advance = QFixed::fromFixed(positions[i].x_advance);
            g.offset.x = QFixed::fromFixed(positions[i].x_offset);
            g.offset.y = QFixed::fromFixed(-positions[i].y_offset);
            g.clusterStart = !i || glyphs[i].cluster != glyphs[i-1].cluster;
            result->glyphs.append(g);
        }
        for (unsigned i = 0; i < n;) {
            unsigned end = i + 1;
            while (end < n && glyphs[end].cluster == glyphs[i].cluster) ++end;
            unsigned charEnd = end < n ? glyphs[end].cluster : unsigned(length);
            for (unsigned c = glyphs[i].cluster; c < charEnd; ++c) result->clusters[from+c] = base+i;
            i = end;
        }
        result->ascent = qMax(result->ascent, actual->ascent());
        result->descent = qMax(result->descent, actual->descent());
        result->leading = qMax(result->leading, actual->leading());
    return true;
}
}

bool qt_shape_myanmar(const QString &text, QFontEngine *font, bool kerning, QMyanmarShape *result)
{
    if (text.isEmpty() || text.size() > USHRT_MAX) return false;
    QFontEngineMulti *multi = font->type() == QFontEngine::Multi ? static_cast<QFontEngineMulti *>(font) : 0;
    QFontEngine *primary = multi ? multi->engine(0) : font;
    FontCache cache;
    Font *primaryFont = cache.get(0, primary);
    if (!hb_face_get_glyph_count(primaryFont->face)) return false;
    Buffer whole;
    if (!shape(text, primaryFont->font, kerning, &whole)) return false;
    unsigned count;
    const hb_glyph_info_t *info = hb_buffer_get_glyph_infos(whole.buffer, &count);
    if (!count || count > USHRT_MAX || info[0].cluster != 0) return false;
    result->clusters.resize(text.size());
    // Preserve complete-run context/kerning when the primary face covers it.
    if (covers(primaryFont->font, text))
        return append(whole.buffer, 0, text.size(), 0, primary, result);
    // Enumerate complete fallback faces, rather than only the faces selected
    // by nominal per-character fallback. One face must cover the whole cluster.
    QVector<int> starts;
    QVector<uint> selectedFonts;
    for (unsigned group = 0; group < count;) {
        unsigned next = group + 1;
        while (next < count && info[next].cluster == info[group].cluster) ++next;
        unsigned from = info[group].cluster;
        unsigned to = next < count ? info[next].cluster : unsigned(text.size());
        if (to <= from || to > unsigned(text.size())) return false;
        const QString cluster = text.mid(from, to - from);
        uint selected = 0;
        for (int index = 0; index < (multi ? multi->myanmarEngineCount() : 1); ++index) {
            QFontEngine *candidate = multi ? multi->myanmarEngine(index) : font;
            if (!candidate || candidate->type() == QFontEngine::Box) continue;
            Font *candidateFont = cache.get(index, candidate);
            if (hb_face_get_glyph_count(candidateFont->face) && covers(candidateFont->font, cluster)) {
                selected = index;
                break;
            }
        }
        starts.append(from);
        selectedFonts.append(selected);
        group = next;
    }
    starts.append(text.size());
    // Shape contiguous clusters using the same face together, preserving
    // contextual substitutions and kerning across their boundaries.
    for (int group = 0; group < selectedFonts.size();) {
        int next = group + 1;
        while (next < selectedFonts.size() && selectedFonts[next] == selectedFonts[group]) ++next;
        uint selected = selectedFonts[group];
        QFontEngine *actual = multi ? multi->myanmarEngine(selected) : font;
        if (!actual) return false;
        const QString span = text.mid(starts[group], starts[next] - starts[group]);
        Buffer part;
        if (!shape(span, cache.get(selected, actual)->font, kerning, &part)) return false;
        if (!append(part.buffer, starts[group], span.size(), selected, actual, result)) return false;
        group = next;
    }
    return true;
}
QT_END_NAMESPACE
