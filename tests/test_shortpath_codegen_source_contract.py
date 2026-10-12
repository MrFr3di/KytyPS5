"""S4-B5-D source-provenance: no production mutations, native benchmarks, 3 CI jobs."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
FILES=("src/graphics/host_gpu/renderer/cache/textureCache.cpp",
       "src/graphics/host_gpu/renderer/cache/canonicalOwnerPageWalk.h",
       "tests/ShortPathCodegenBench.cpp",
       "tests/ShortPathCodegenProbe.cpp",
       "CMakeLists.txt",
       ".github/workflows/texture-range-index.yml")
def source():return [(ROOT/p).read_text(encoding="utf-8") for p in FILES]
def audit(c,h,b,probe,cm,w):
    beg=c.find("TextureCache::ImageIds TextureCache::FindImagesInRegion(")
    end=c.find("ImageId TextureCache::GetNullImage(",beg)
    if beg<0 or end<=beg:return False
    f=c[beg:end]
    for v in ("++m_image_query_epoch","image->query_epoch == query_epoch",
              "image->query_epoch = query_epoch;","image->Overlaps(address, size, page_overlap)",
              "ForEachPageCountCanonicalOwnerPage<256>(m_image_page_table, pages, visit_page);",
              "ForEachCanonicalOwnerPage<false>(m_image_page_table, pages, visit_page);"):
        if v not in f:return False
    for v in ("ShouldSkipCanonicalBucketsByPageCount<MinimumPages, PageTable>(pages)",
              "ForEachCanonicalOwnerPage<true>(table, pages",
              "ForEachCanonicalOwnerPage<false>(table, pages",
              "table.Find(page)","table.ForEachAllocatedPageInRange("):
        if v not in h:return False
    for v in ("using Id=Common::SlotId;","Common::SlotVector<Image> images;",
              "Pages pages;","Oracle(const Query&","generation reuse",
              "effective mutation live","empty allocated bucket retained",
              "ForEachCanonicalOwnerPage<false>(pages,range,visit)",
              "ForEachPageCountCanonicalOwnerPage<256>(pages,range,visit)",
              "ShouldSkipCanonicalBucketsByPageCount<256,Pages>(range)",
              "ForEachPageCountCanonicalOwnerPage<1024>(pages,range,visit)",
              "ForEachCanonicalOwnerPage<true>(pages,range,visit)",
              "constexpr size_t kBatch[]={8,512,2048,2048,128,128}"):
        if v not in b:return False
    # Both the direct legacy helper and manual adaptive-false fallback
    # must survive; checking only substring presence misses deleting one.
    if b.count("ForEachCanonicalOwnerPage<false>(pages,range,visit)")!=2:return False
    for v in ("KytyB5DDirectFind","KytyB5DCanonicalLegacy",
              "KytyB5DPageOnly256","KytyB5DManual256",
              "KYTY_B5D_NOINLINE"):
        if v not in probe:return False
    if 'option(KYTY_TEXTURE_CANONICAL_BUCKET_SKIP "Opt-in canonical L1 bucket skip for TextureCache region lookup" OFF)' not in cm:return False
    if "target_compile_definitions(kyty_emulator PRIVATE KYTY_TEXTURE_CANONICAL_BUCKET_SKIP=1)" not in cm:return False
    if "ShortPathCodegenBench.cpp" not in cm or "shortpath_codegen_bench" not in cm:return False
    for v in ("-DKYTY_TEXTURE_CANONICAL_BUCKET_SKIP=ON",
              "shortpath-fast.csv","shortpath-linux.csv","shortpath.csv",
              "test_shortpath_codegen_driver.py","test_shortpath_codegen_asm.py",
              "ShortPathCodegenProbe.cpp","shortpath-codegen-fast.obj",
              "analyze_shortpath_codegen.py"):
        if v not in w:return False
    # Three independent jobs must each independently enforce the expected result.
    if w.count("S4_B5D_SHORTPATH_PASS")!=3:return False
    if w.count("S4_B5D_CODEGEN_PASS")!=1:return False
    return True
class Tests(unittest.TestCase):
    def test_original(self):self.assertTrue(audit(*source()))
    def mutant(self,n,a,b):
        x=source()
        self.assertIn(a,x[n])
        x[n]=x[n].replace(a,b,1)
        self.assertFalse(audit(*x))
    def test_wrong_overlap(self):
        self.mutant(0,"image->Overlaps(address, size, page_overlap)","true")
    def test_off_default(self):
        self.mutant(4,'for TextureCache region lookup" OFF)',
                       'for TextureCache region lookup" ON)')
    def test_no_legacy(self):
        self.mutant(2,"ForEachCanonicalOwnerPage<false>(pages,range,visit)",
                       "ForEachCanonicalOwnerPage<true>(pages,range,visit)")
    def test_no_manual_t256(self):
        self.mutant(2,"ShouldSkipCanonicalBucketsByPageCount<256,Pages>(range)",
                       "true")
    def test_no_native(self):
        self.mutant(2,"using Id=Common::SlotId;","using Id=uint64_t;")
    def test_no_codegen_probe(self):
        self.mutant(3,"KytyB5DPageOnly256","BadlyRenamedProbe")
    def test_removed_runner(self):
        self.mutant(5,"S4_B5D_SHORTPATH_PASS","MISSING_EVIDENCE")
    def test_removed_codegen_gate(self):
        self.mutant(5,"S4_B5D_CODEGEN_PASS","NO_ASM")
if __name__=="__main__":unittest.main()
