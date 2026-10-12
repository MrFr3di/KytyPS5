"""S4-B5-B source-bound production, oracle, CMake and CI provenance."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
FILES=(
    "src/graphics/host_gpu/renderer/cache/textureCache.cpp",
    "src/graphics/host_gpu/renderer/cache/canonicalOwnerPageWalk.h",
    "tests/AdaptiveCanonicalBucketBench.cpp",
    "CMakeLists.txt",
    ".github/workflows/texture-range-index.yml",
)
def files():
    return [((ROOT/name).read_text(encoding="utf-8")) for name in FILES]
def audit(c,h,b,cm,w):
    start=c.find("TextureCache::ImageIds TextureCache::FindImagesInRegion(")
    end=c.find("ImageId TextureCache::GetNullImage(",start)
    if start<0 or end<start:return False
    p=c[start:end]
    for token in ("ImagePageTable::TryGetPageRange(address, size, pages)",
        "++m_image_query_epoch","image.query_epoch = 0;",
        "image->query_epoch == query_epoch","image->query_epoch = query_epoch;",
        "image->Overlaps(address, size, page_overlap)",
        "#if defined(KYTY_TEXTURE_CANONICAL_BUCKET_SKIP) && KYTY_TEXTURE_CANONICAL_BUCKET_SKIP",
        "ForEachAdaptiveCanonicalOwnerPage<256>(m_image_page_table, pages, visit_page);",
        "ForEachCanonicalOwnerPage<false>(m_image_page_table, pages, visit_page);",
        "m_range_telemetry.OnQuery(pages.last_exclusive - pages.first"): 
        if token not in p:return False
    for token in ("pages.first >= pages.last_exclusive",
       "pages.last_exclusive > PageTable::kPageCount",
       "pages.last_exclusive - pages.first >= MinimumPages",
       "(pages.last_exclusive - 1) / PageTable::kBucketEntries",
       "ForEachCanonicalOwnerPage<true>(table, pages",
       "ForEachCanonicalOwnerPage<false>(table, pages"):
        if token not in h:return False
    for token in ("common/slotVector.h","multiLevelPageTable.h","Oracle(const Query&",
       "generation reuse","effective mutation live","empty allocated bucket retained",
       "ForEachAdaptiveCanonicalOwnerPage<Threshold>",
       "Run<2,16>","Run<2,64>","Run<2,256>","Run<2,1024>"):
        if token not in b:return False
    for token in ('option(KYTY_TEXTURE_CANONICAL_BUCKET_SKIP "Opt-in canonical L1 bucket skip for TextureCache region lookup" OFF)',
      "target_compile_definitions(kyty_emulator PRIVATE KYTY_TEXTURE_CANONICAL_BUCKET_SKIP=1)",
      "AdaptiveCanonicalBucketBench.cpp","adaptive_canonical_walk_bench"):
        if token not in cm:return False
    for token in ("-DKYTY_TEXTURE_CANONICAL_BUCKET_SKIP=ON",
      "run_adaptive_canonical_walk.py","S4_B5B_ADAPTIVE_PASS",
      "adaptive-canonical-fast.csv","adaptive-canonical-linux.csv",
      "adaptive-canonical.csv"):
        if token not in w:return False
    return True

class SourceTests(unittest.TestCase):
    def test_real(self):self.assertTrue(audit(*files()))
    def mutate(self,n,a,b):
        arr=files()
        self.assertIn(a,arr[n])
        arr[n]=arr[n].replace(a,b,1)
        self.assertFalse(audit(*arr))
    def test_wrong_threshold(self):
        self.mutate(0,"ForEachAdaptiveCanonicalOwnerPage<256>",
                       "ForEachAdaptiveCanonicalOwnerPage<16>")
    def test_no_fallback(self):
        self.mutate(0,"ForEachCanonicalOwnerPage<false>","ForEachCanonicalOwnerPage<true>")
    def test_bad_page_bound(self):
        self.mutate(1,"pages.last_exclusive - pages.first >= MinimumPages",
                       "true")
    def test_bad_bucket_boundary(self):
        self.mutate(1,"(pages.last_exclusive - 1) / PageTable::kBucketEntries",
                       "(pages.last_exclusive) / PageTable::kBucketEntries")
    def test_no_native_oracle(self):self.mutate(2,"Oracle(const Query&","Stub(const Query&")
    def test_on_by_default(self):
        self.mutate(3,'for TextureCache region lookup" OFF)',
                       'for TextureCache region lookup" ON)')
    def test_uncompiled_emulator_path(self):
        self.mutate(3,"target_compile_definitions(kyty_emulator PRIVATE KYTY_TEXTURE_CANONICAL_BUCKET_SKIP=1)",
                       "message(STATUS \"opt-in only\")")
    def test_no_windows_flag(self):
        self.mutate(4,"-DKYTY_TEXTURE_CANONICAL_BUCKET_SKIP=ON",
                       "-DKYTY_TEXTURE_CANONICAL_BUCKET_SKIP=OFF")
if __name__=="__main__":unittest.main()
