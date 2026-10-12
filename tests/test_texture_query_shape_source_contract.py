"""S4-B5-F: negative source contract for runtime-off, no-address telemetry."""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[1]
FILES=("src/graphics/host_gpu/renderer/cache/textureCache.cpp",
       "src/graphics/host_gpu/renderer/cache/textureRangeTelemetry.h",
       "tests/TextureRangeTelemetryTests.cpp",
       "scripts/analyze_texture_query_shape.py",
       ".github/workflows/texture-range-index.yml",
       "CMakeLists.txt")
def source():return [(ROOT/p).read_text(encoding="utf-8") for p in FILES]
def audit(c,h,t,a,w,cm):
    beg=c.find("TextureCache::ImageIds TextureCache::FindImagesInRegion(")
    end=c.find("ImageId TextureCache::GetNullImage(",beg)
    if beg<0 or end<=beg:return False
    region=c[beg:end]
    for val in ("ImagePageTable::TryGetPageRange(address, size, pages)",
        "if (m_range_telemetry_enabled) {",
        "ForEachCanonicalOwnerPage<false>(m_image_page_table, pages, visit_page);",
        "ForEachPageCountCanonicalOwnerPage<256>(m_image_page_table, pages, visit_page);",
        "image->Overlaps(address, size, page_overlap)",
        "m_range_telemetry.OnQuery(pages.last_exclusive - pages.first, page_overlap,",
        "(void)m_range_telemetry.ObserveCanonicalSpan(",
        "pages.first, pages.last_exclusive, ImagePageTable::kBucketEntries,",
        "ImagePageTable::kPageCount);"):
        if val not in region:return False
    if region.index("m_range_telemetry.OnQuery(")>region.index("(void)m_range_telemetry.ObserveCanonicalSpan("):
        return False
    if c.count('std::getenv("KYTY_TEXTURE_RANGE_TRACE")')!=1:return False
    if "m_range_telemetry_enabled = enabled != nullptr && std::strcmp(enabled, \"1\") == 0;" not in c:return False
    if "m_range_telemetry.WriteCsv(file);" not in c:return False
    for val in ("if (bucket_entries == 0 || first >= last_exclusive ||",
        "last_exclusive > page_count)","last_exclusive - first",
        "(first / bucket_entries) != ((last_exclusive - 1) / bucket_entries)",
        "pages >= 64","pages >= 256","pages >= 1024",
        "pages >= 256 && !crosses_l1","pages >= 256 && crosses_l1",
        "shape_queries", "shape_l1_cross_queries",
        'emit("shape_pages_64plus",','emit("shape_pages_256plus",',
        'emit("shape_pages_1024plus",',
        'emit("shape_pages_256plus_same_l1",',
        'emit("shape_pages_256plus_cross_l1",'):
        if val not in h:return False
    if "#include <vector>" in h or "std::vector<" in h:return False
    for val in ("--shape-fixture","TestCanonicalSpanShape();",
                "MakeShapeFixture().WriteCsv(stdout);",
                "reject beyond 44-bit address space",
                "page threshold boundaries","L1-cross counts"):
        if val not in t:return False
    if "BLOCKED_NO_REPRESENTATIVE_GAME_TRACE" not in a or "shape_queries" not in a:return False
    for val in ("-DKYTY_TEXTURE_RANGE_TELEMETRY=ON",
                "test_texture_query_shape_analyzer.py",
                "test_texture_query_shape_source_contract.py",
                "analyze_texture_query_shape.py","--shape-fixture",
                "texture-shape-linux.csv","texture-shape-fast.csv",
                "texture-shape.csv"):
        if val not in w:return False
    if w.count("S4_B5F_SHAPE_PASS")!=3:return False
    if 'option(KYTY_TEXTURE_RANGE_TELEMETRY "Compile optional TextureCache range statistics" OFF)' not in cm:return False
    if 'option(KYTY_TEXTURE_CANONICAL_BUCKET_SKIP "Opt-in canonical L1 bucket skip for TextureCache region lookup" OFF)' not in cm:return False
    return True
class Tests(unittest.TestCase):
    def test_original(self):self.assertTrue(audit(*source()))
    def mutant(self,n,a,b):
        s=source()
        self.assertIn(a,s[n])
        s[n]=s[n].replace(a,b,1)
        self.assertFalse(audit(*s))
    def test_no_query_shape(self):
        self.mutant(0,"(void)m_range_telemetry.ObserveCanonicalSpan(",
                       "(void)m_range_telemetry.UnverifiedSpan(")
    def test_bad_range_validation(self):
        self.mutant(1,"last_exclusive > page_count)","false)")
    def test_no_crossing(self):
        self.mutant(1,"pages >= 256 && crosses_l1","pages >= 256")
    def test_default_on(self):
        self.mutant(5,"range statistics\" OFF)","range statistics\" ON)")
    def test_missing_host(self):
        self.mutant(4,"S4_B5F_SHAPE_PASS","INVALID_PASS")
    def test_fake_fixture(self):
        self.mutant(2,"--shape-fixture","--real-game")
if __name__=="__main__":unittest.main()
