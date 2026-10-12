"""S4-B5-C source-bound negative mutation and integration provenance."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
PATHS=("src/graphics/host_gpu/renderer/cache/textureCache.cpp",
       "src/graphics/host_gpu/renderer/cache/canonicalOwnerPageWalk.h",
       "tests/PageCountCanonicalPolicyBench.cpp",
       "CMakeLists.txt",".github/workflows/texture-range-index.yml")
def source():return [(ROOT/p).read_text(encoding="utf-8") for p in PATHS]
def audit(c,h,b,cm,w):
    start=c.find("TextureCache::ImageIds TextureCache::FindImagesInRegion(")
    stop=c.find("ImageId TextureCache::GetNullImage(",start)
    if start<0 or stop<=start:return False
    f=c[start:stop]
    for v in ("ImagePageTable::TryGetPageRange(address, size, pages)",
         "++m_image_query_epoch","image.query_epoch = 0;",
         "image->query_epoch == query_epoch","image->query_epoch = query_epoch;",
         "image->Overlaps(address, size, page_overlap)","result.push_back(id);",
         "#if defined(KYTY_TEXTURE_CANONICAL_BUCKET_SKIP) && KYTY_TEXTURE_CANONICAL_BUCKET_SKIP",
         "ForEachPageCountCanonicalOwnerPage<256>(m_image_page_table, pages, visit_page);",
         "ForEachCanonicalOwnerPage<false>(m_image_page_table, pages, visit_page);",
         "m_range_telemetry.OnQuery(pages.last_exclusive - pages.first"):
        if v not in f:return False
    for v in ("pages.first < pages.last_exclusive",
         "pages.last_exclusive <= PageTable::kPageCount",
         "pages.last_exclusive - pages.first >= MinimumPages;",
         "ShouldSkipCanonicalBucketsByPageCount<MinimumPages, PageTable>(pages)",
         "ForEachCanonicalOwnerPage<true>(table, pages",
         "ForEachCanonicalOwnerPage<false>(table, pages",
         "table.Find(page)","table.ForEachAllocatedPageInRange("):
        if v not in h:return False
    for v in ("common/slotVector.h","multiLevelPageTable.h","Oracle(const Query&",
        "generation reuse","effective mutation live","empty allocated bucket retained",
        "ShouldSkipCanonicalBucketsByPageCount<256,Pages>",
        "ForEachPageCountCanonicalOwnerPage<Threshold>",
        "Run<2,256>","Run<3,64>","Run<3,256>","Run<3,1024>",
        "constexpr size_t kBatch[]={8,4,8,128,256,128,128}"):
        if v not in b:return False
    if 'option(KYTY_TEXTURE_CANONICAL_BUCKET_SKIP "Opt-in canonical L1 bucket skip for TextureCache region lookup" OFF)' not in cm:return False
    if "target_compile_definitions(kyty_emulator PRIVATE KYTY_TEXTURE_CANONICAL_BUCKET_SKIP=1)" not in cm:return False
    if "PageCountCanonicalPolicyBench.cpp" not in cm or "page_count_policy_bench" not in cm:return False
    for v in ("-DKYTY_TEXTURE_CANONICAL_BUCKET_SKIP=ON","run_page_count_policy.py",
      "page-policy-fast.csv","page-policy-linux.csv","page-policy.csv"):
        if v not in w:return False
    # One distinct strict PASS sentinel on each of the three existing jobs.
    # Replacing or deleting any one must be rejected, not masked by another.
    if w.count("S4_B5C_PAGE_COUNT_PASS") != 3:return False
    return True
class SourceTests(unittest.TestCase):
    def test_original(self):self.assertTrue(audit(*source()))
    def mutant(self,n,a,b):
        docs=source()
        self.assertIn(a,docs[n])
        docs[n]=docs[n].replace(a,b,1)
        self.assertFalse(audit(*docs))
    def test_no_fallback(self):
        self.mutant(0,"ForEachCanonicalOwnerPage<false>(m_image_page_table, pages, visit_page);",
                       "ForEachCanonicalOwnerPage<true>(m_image_page_table, pages, visit_page);")
    def test_no_on_path(self):
        self.mutant(0,"ForEachPageCountCanonicalOwnerPage<256>(m_image_page_table, pages, visit_page);",
                       "ForEachAdaptiveCanonicalOwnerPage<256>(m_image_page_table, pages, visit_page);")
    def test_wrong_live_overlap(self):
        self.mutant(0,"image->Overlaps(address, size, page_overlap)","true")
    def test_wrong_page_size(self):
        self.mutant(1,"pages.last_exclusive - pages.first >= MinimumPages;","true;")
    def test_unchecked_range(self):
        self.mutant(1,"pages.last_exclusive <= PageTable::kPageCount","true")
    def test_wrong_oracle(self):self.mutant(2,"Oracle(const Query&","Fake(const Query&")
    def test_no_batch(self):
        self.mutant(2,"constexpr size_t kBatch[]={8,4,8,128,256,128,128}",
                       "constexpr size_t kBatch[]={1,1,1,1,1,1,1}")
    def test_on_default(self):
        self.mutant(3,'for TextureCache region lookup" OFF)',
                       'for TextureCache region lookup" ON)')
    def test_emulator_define(self):
        self.mutant(3,"target_compile_definitions(kyty_emulator PRIVATE KYTY_TEXTURE_CANONICAL_BUCKET_SKIP=1)",
                       'message(STATUS "no compiled opt-in")')
    def test_missing_runner(self):
        self.mutant(4,"S4_B5C_PAGE_COUNT_PASS","UNVERIFIED")
if __name__=="__main__":unittest.main()
