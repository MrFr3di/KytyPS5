"""S4-B4b source tripwire: production native canonical and matched sidecar."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[1]
CANON=ROOT/"src/graphics/host_gpu/renderer/cache/textureCache.cpp"
SIDECAR=ROOT/"src/graphics/host_gpu/renderer/cache/nativeLazyRangeSidecar.h"
BENCH=ROOT/"tests/NativeMatchedRangeBench.cpp"
WALK=ROOT/"src/graphics/host_gpu/renderer/cache/canonicalOwnerPageWalk.h"

def read():
    return (CANON.read_text(encoding="utf8"),SIDECAR.read_text(encoding="utf8"),
            BENCH.read_text(encoding="utf8"),WALK.read_text(encoding="utf8"))

def audit(c,s,b,w):
    start=c.find("TextureCache::ImageIds TextureCache::FindImagesInRegion(")
    stop=c.find("\nImageId TextureCache::GetNullImage(",start)
    if start<0 or stop<=start:return False
    fn=c[start:stop]
    # S4-B5-B preserved S4-B4b native-provenance semantics: the canonical
    # nonallocating Find remains OFF, adaptive ON is still canonical-only.
    # Verify both compiled production callsites and the real fallback body.
    if ("ForEachAdaptiveCanonicalOwnerPage<256>(m_image_page_table, pages, visit_page);" not in fn or
        "ForEachCanonicalOwnerPage<false>(m_image_page_table, pages, visit_page);" not in fn or
        "table.Find(page)" not in w or
        "ForEachCanonicalOwnerPage<false>(table, pages" not in w):
        return False
    for key in ("image->query_epoch == query_epoch",
                "image->query_epoch = query_epoch;",
                "image->Overlaps(address, size, page_overlap)",
                "result.push_back(id);"):
        if key not in fn:return False
    for key in ("class NativeLazyRangeSidecar final",
                "bool Register(Id id, uint64_t address, uint64_t bytes)",
                "bool Unregister(Id id)",
                "bool TryFind(uint64_t address, uint64_t bytes, bool page_overlap",
                "if (!Build()) return false;",
                "output = std::move(result);"):
        if key not in s:return False
    for key in ("using Id=Common::SlotId;",
                "Common::SlotVector<Image> images;",
                "Table canonical;",
                "Side sidecar;",
                "canonical[p].push_back(id);",
                "sidecar.Register(id,s.begin,s.bytes)",
                "return Canonical(q);",
                "image->query_epoch==epoch",
                "image->query_epoch=epoch;",
                "Matches(*image,q.begin,q.bytes,q.rounded)",
                "Matches(*im,begin,bytes,rounded)",
                "sidecar.Unregister(id)",
                "idx.images.try_get(old)==nullptr",
                "const auto expected=canonical.IndependentOracle(q);",
                "Same(b,expected)",
                "row.registration=Time(",
                "row.churn=Time(",
                "track=true;",
                "track=false;"):
        if key not in b:return False
    if "++fallback;\n    return Canonical(q);" not in b:return False
    if "canonical_plus_native_lazy" not in b or "canonical_only" not in b:return False
    if "S4-B4b" not in b:return False
    return True

class NativeMatchedSourceContractTests(unittest.TestCase):
    def test_source(self):self.assertTrue(audit(*read()))
    def wrong(self,idx,old,new):
        v=list(read())
        self.assertIn(old,v[idx])
        v[idx]=v[idx].replace(old,new,1)
        self.assertFalse(audit(*v))
    def test_missing_legacy_off(self):
        self.wrong(0,"ForEachCanonicalOwnerPage<false>(m_image_page_table, pages, visit_page);",
                   "ForEachCanonicalOwnerPage<true>(m_image_page_table, pages, visit_page);")
    def test_missing_adaptive_on(self):
        self.wrong(0,"ForEachAdaptiveCanonicalOwnerPage<256>(m_image_page_table, pages, visit_page);",
                   "ForEachCanonicalOwnerPage<true>(m_image_page_table, pages, visit_page);")
    def test_missing_production_dedup(self):
        self.wrong(0,"image->query_epoch == query_epoch","image->query_epoch != query_epoch")
    def test_no_sidecar_fallback(self):
        self.wrong(2,"++fallback;\n    return Canonical(q);",
                   "++fallback;\n    return result;")
    def test_non_native_id(self):
        self.wrong(2,"using Id=Common::SlotId;","using Id=uint32_t;")
    def test_skip_canonical_registration(self):
        self.wrong(2,"canonical[p].push_back(id);","// no registration")
    def test_wrong_live_match(self):
        self.wrong(2,"Matches(*im,begin,bytes,rounded)","true")
    def test_missing_generation_reuse_check(self):
        self.wrong(2,"idx.images.try_get(old)==nullptr","true")
    def test_no_independent_oracle(self):
        self.wrong(2,"const auto expected=canonical.IndependentOracle(q);",
                   "const Owners expected;")

if __name__=="__main__":unittest.main()
