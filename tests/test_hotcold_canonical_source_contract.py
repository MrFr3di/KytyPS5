"""KYTY-INDEX-001 S4-B5-E guarded research-only source provenance."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
PATHS=("src/graphics/host_gpu/renderer/cache/textureCache.cpp",
       "src/graphics/host_gpu/renderer/cache/canonicalOwnerPageWalk.h",
       "tests/HotColdCanonicalBench.cpp",
       "tests/HotColdCodegenProbe.cpp",
       "CMakeLists.txt",".github/workflows/texture-range-index.yml")
def contents():return [(ROOT/x).read_text(encoding="utf-8") for x in PATHS]
def audit(c,h,b,probe,cm,w):
    beg=c.find("TextureCache::ImageIds TextureCache::FindImagesInRegion(")
    end=c.find("ImageId TextureCache::GetNullImage(",beg)
    if beg<0 or end<=beg:return False
    body=c[beg:end]
    for token in ("ImagePageTable::TryGetPageRange(address, size, pages)",
                  "image->query_epoch == query_epoch",
                  "image->query_epoch = query_epoch;",
                  "image->Overlaps(address, size, page_overlap)",
                  "ForEachPageCountCanonicalOwnerPage<256>(m_image_page_table, pages, visit_page);",
                  "ForEachCanonicalOwnerPage<false>(m_image_page_table, pages, visit_page);"):
        if token not in body:return False
    for token in ("ShouldSkipCanonicalBucketsByPageCount<MinimumPages, PageTable>(pages)",
                  "ForEachCanonicalOwnerPage<true>(table, pages",
                  "ForEachCanonicalOwnerPage<false>(table, pages",
                  "table.Find(page)","table.ForEachAllocatedPageInRange("):
        if token not in h:return False
    for token in ("using Id=Common::SlotId;","Common::SlotVector<Image> images;",
                  "Pages pages;","Oracle(const Query&","generation reuse",
                  "effective mutation live","empty allocated bucket retained",
                  "KYTY_E_NOINLINE void ColdSkipArm(",
                  "KYTY_E_NOINLINE bool ColdEntireDispatcher(",
                  "ColdSkipArm(pages,range,visit);",
                  "ColdEntireDispatcher(pages,range,visit)",
                  "ForEachPageCountCanonicalOwnerPage<256>(pages,range,visit)",
                  "ShouldSkipCanonicalBucketsByPageCount<256,Pages>(range)",
                  "ForEachCanonicalOwnerPage<false>(pages,range,visit)",
                  "ForEachCanonicalOwnerPage<true>(pages,range,visit)",
                  "constexpr size_t kBatch[]={8,512,2048,2048,128,128}"):
        if token not in b:return False
    if b.count("ColdSkipArm(pages,range,visit);")!=1:return False
    if b.count("ColdEntireDispatcher(pages,range,visit)")!=1:return False
    for token in ("KytyB5ELegacy","KytyB5EInline256","KytyB5EColdSkip",
                  "KytyB5EHotCold256","KytyB5EFullDispatcher256",
                  "KYTY_E_NOINLINE","ShouldSkipCanonicalBucketsByPageCount<256,Table>(r)"):
        if token not in probe:return False
    if 'option(KYTY_TEXTURE_CANONICAL_BUCKET_SKIP "Opt-in canonical L1 bucket skip for TextureCache region lookup" OFF)' not in cm:return False
    if "target_compile_definitions(kyty_emulator PRIVATE KYTY_TEXTURE_CANONICAL_BUCKET_SKIP=1)" not in cm:return False
    if "HotColdCanonicalBench.cpp" not in cm or "hotcold_canonical_bench" not in cm:return False
    for token in ("-DKYTY_TEXTURE_CANONICAL_BUCKET_SKIP=ON",
                  "HotColdCanonicalBench.cpp","HotColdCodegenProbe.cpp",
                  "analyze_hotcold_codegen.py","test_hotcold_canonical_driver.py",
                  "hotcold-fast.csv","hotcold-linux.csv","hotcold.csv",
                  "hotcold-codegen-fast.obj","hotcold-codegen-summary-fast.csv"):
        if token not in w:return False
    if w.count("S4_B5E_HOTCOLD_PASS")!=3:return False
    if w.count("S4_B5E_CODEGEN_PASS")!=1:return False
    return True
class Tests(unittest.TestCase):
    def test_source(self):self.assertTrue(audit(*contents()))
    def mutated(self,n,a,b):
        data=contents()
        self.assertIn(a,data[n])
        data[n]=data[n].replace(a,b,1)
        self.assertFalse(audit(*data))
    def test_wrong_overlap(self):
        self.mutated(0,"image->Overlaps(address, size, page_overlap)","true")
    def test_false_path_removed(self):
        self.mutated(2,"ColdSkipArm(pages,range,visit);","// removed")
    def test_no_noinline(self):
        self.mutated(2,"KYTY_E_NOINLINE void ColdSkipArm(","void ColdSkipArm(")
    def test_full_dispatch_missing(self):
        self.mutated(2,"ColdEntireDispatcher(pages,range,visit)","true")
    def test_no_generation(self):
        self.mutated(2,"generation reuse","generation ignored")
    def test_wrong_compiled_optin(self):
        self.mutated(0,"ForEachPageCountCanonicalOwnerPage<256>(m_image_page_table, pages, visit_page);",
                          "ForEachCanonicalOwnerPage<true>(m_image_page_table, pages, visit_page);")
    def test_default_off(self):
        self.mutated(4,'for TextureCache region lookup" OFF)',
                          'for TextureCache region lookup" ON)')
    def test_missing_runner(self):
        self.mutated(5,"S4_B5E_HOTCOLD_PASS","REJECTED")
    def test_missing_disassembly(self):
        self.mutated(5,"S4_B5E_CODEGEN_PASS","REJECTED")
if __name__=="__main__":unittest.main()
