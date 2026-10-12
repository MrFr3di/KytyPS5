"""S4-B5-A: source provenance for production OFF default/opt-in page walk."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
PROD=ROOT/"src/graphics/host_gpu/renderer/cache/textureCache.cpp"
HDR=ROOT/"src/graphics/host_gpu/renderer/cache/canonicalOwnerPageWalk.h"
PAGES=ROOT/"src/graphics/host_gpu/renderer/cache/multiLevelPageTable.h"
CMAKE=ROOT/"CMakeLists.txt"
WORKFLOW=ROOT/".github/workflows/texture-range-index.yml"

def source():return [p.read_text(encoding="utf-8") for p in (PROD,HDR,PAGES,CMAKE,WORKFLOW)]
def audit(c,h,p,cm,w):
    start=c.find("TextureCache::ImageIds TextureCache::FindImagesInRegion(")
    end=c.find("ImageId TextureCache::GetNullImage(",start)
    if start<0 or end<=start:return False
    f=c[start:end]
    for phrase in (
      "ImagePageTable::TryGetPageRange(address, size, pages)",
      "++m_image_query_epoch",
      "image.query_epoch = 0;",
      "image->query_epoch == query_epoch",
      "image->query_epoch = query_epoch;",
      "image->Overlaps(address, size, page_overlap)",
      "m_range_telemetry.OnQuery(pages.last_exclusive - pages.first",
      "#if defined(KYTY_TEXTURE_CANONICAL_BUCKET_SKIP) && KYTY_TEXTURE_CANONICAL_BUCKET_SKIP",
      "ForEachPageCountCanonicalOwnerPage<256>(m_image_page_table, pages, visit_page);",
      "ForEachCanonicalOwnerPage<false>(m_image_page_table, pages, visit_page);",
      "const auto visit_page =",
      "owners.ForEach([&](ImageId id)",
    ):
        if phrase not in f:return False
    if "m_image_page_table[page].push_back(id);" not in c:return False
    if "owners->Erase(id)" not in c:return False
    if "template <bool SkipNeverAllocatedBuckets, class PageTable, class Callback>" not in h:
        return False
    for phrase in ("if constexpr (SkipNeverAllocatedBuckets)",
      "table.ForEachAllocatedPageInRange(", "table.Find(page)",
      "ShouldSkipCanonicalBuckets<MinimumPages, PageTable>(pages)",
      "ForEachCanonicalOwnerPage<true>(table, pages",
      "ForEachCanonicalOwnerPage<false>(table, pages",
      "if (!owners.empty()) callback(page,owners)",
      "owners!=nullptr && !owners->empty()"):
        if phrase not in h:return False
    for phrase in ("void ForEachAllocatedPageInRange(", "if (bucket == nullptr)",
                   "func(page, (*bucket)[page - bucket_begin]);"):
        if phrase not in p:return False
    if 'option(KYTY_TEXTURE_CANONICAL_BUCKET_SKIP "Opt-in canonical L1 bucket skip for TextureCache region lookup" OFF)' not in cm:
        return False
    if "target_compile_definitions(kyty_emulator PRIVATE KYTY_TEXTURE_CANONICAL_BUCKET_SKIP=1)" not in cm:
        return False
    if "-DKYTY_TEXTURE_CANONICAL_BUCKET_SKIP=ON" not in w:return False
    return True

class SourceTests(unittest.TestCase):
    def test_real(self):self.assertTrue(audit(*source()))
    def mutant(self,n,a,b):
        s=source()
        self.assertIn(a,s[n])
        s[n]=s[n].replace(a,b,1)
        self.assertFalse(audit(*s))
    def test_wrong_native_overlap(self):
        self.mutant(0,"image->Overlaps(address, size, page_overlap)","true")
    def test_no_epoch(self):
        self.mutant(0,"image->query_epoch == query_epoch","false")
    def test_no_fallback_find(self):
        self.mutant(1,"table.Find(page)","table.GetOrCreate(page)")
    def test_no_sparse_bucket_skip(self):
        self.mutant(1,"table.ForEachAllocatedPageInRange(","table.ForEachOtherPageInRange(")
    def test_off_by_default(self):
        self.mutant(3,'for TextureCache region lookup" OFF)','for TextureCache region lookup" ON)')
    def test_gating_removed(self):
        self.mutant(0,"#if defined(KYTY_TEXTURE_CANONICAL_BUCKET_SKIP) && KYTY_TEXTURE_CANONICAL_BUCKET_SKIP",
                       "#if defined(KYTY_TEXTURE_CANONICAL_BUCKET_SKIP)")
    def test_removed_page_count_gate(self):
        self.mutant(0,"ForEachPageCountCanonicalOwnerPage<256>(",
                       "ForEachCanonicalOwnerPage<true>(")
    def test_loses_original_register(self):
        self.mutant(0,"m_image_page_table[page].push_back(id);","// dropped native owner")
if __name__=="__main__":unittest.main()
