//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2015 VMware, Inc. or its affiliates.
//
//	@filename:
//		CPhysicalDynamicScan.cpp
//
//	@doc:
//		Base class for physical dynamic scan operators
//
//	@owner:
//
//
//	@test:
//
//---------------------------------------------------------------------------

#include "gpopt/operators/CPhysicalDynamicScan.h"

#include "gpopt/base/CDrvdPropCtxtPlan.h"
#include "gpopt/base/CUtils.h"
#include "gpopt/metadata/CName.h"
#include "gpopt/metadata/CTableDescriptor.h"
#include "gpopt/operators/CExpressionHandle.h"

using namespace gpopt;
using namespace gpos;

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalDynamicScan::CPhysicalDynamicScan
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CPhysicalDynamicScan::CPhysicalDynamicScan(
	CMemoryPool *mp, CTableDescriptor *ptabdesc, ULONG ulOriginOpId,
	const CName *pnameAlias, ULONG scan_id, CColRefArray *pdrgpcrOutput,
	CColRef2dArray *pdrgpdrgpcrParts, IMdIdArray *partition_mdids,
	ColRefToUlongMapArray *root_col_mapping_per_part)
	: CPhysicalScan(mp, pnameAlias, ptabdesc, pdrgpcrOutput),
	  m_ulOriginOpId(ulOriginOpId),
	  m_scan_id(scan_id),
	  m_pdrgpdrgpcrPart(pdrgpdrgpcrParts),
	  m_partition_mdids(partition_mdids),
	  m_root_col_mapping_per_part(root_col_mapping_per_part)

{

	    // --- helper: usage enum -> string ---
	auto UsageToSz = [](CColRef::EUsedStatus us) -> const WCHAR * {
		switch (us)
		{
			case CColRef::EUsed:     return GPOS_WSZ_LIT("EUsed");
			case CColRef::EUnused:   return GPOS_WSZ_LIT("EUnused");
			case CColRef::EUnknown:  return GPOS_WSZ_LIT("EUnknown");
			case CColRef::ESentinel: return GPOS_WSZ_LIT("ESentinel");
		}
		// Defensive (should never reach here)
		return GPOS_WSZ_LIT("E<?>");
	};

    {
        CAutoTrace at(mp);
        at.Os() << GPOS_WSZ_LIT("[WRV-DYN-SCAN] ---- CONSTRUCT ----") << std::endl;
        at.Os() << GPOS_WSZ_LIT("[WRV-DYN-SCAN] scan_id=") << (ULONG) m_scan_id
                << GPOS_WSZ_LIT(" origin_op_id=") << (ULONG) m_ulOriginOpId
                << GPOS_WSZ_LIT(" rel_oid=")
                << CMDIdGPDB::CastMdid(ptabdesc->MDId())->Oid()
                << GPOS_WSZ_LIT(" alias=") << pnameAlias->Pstr()->GetBuffer()
                << std::endl;

        at.Os() << GPOS_WSZ_LIT("[WRV-DYN-SCAN] output_col_count=")
                << (ULONG)(pdrgpcrOutput ? pdrgpcrOutput->Size() : 0)
                << std::endl;

        if (pdrgpcrOutput)
        {
            for (ULONG i = 0; i < pdrgpcrOutput->Size(); ++i)
            {
                const CColRef *c = (*pdrgpcrOutput)[i];
                const CColumnDescriptor *pcd = ptabdesc->Pcoldesc(i);
                BOOL is_sys = c->IsSystemCol();
                BOOL is_dist = c->IsDistCol();
                at.Os()
                    << GPOS_WSZ_LIT("[WRV-DYN-SCAN] OUT idx=") << (ULONG) i
                    << GPOS_WSZ_LIT(" colid=") << (ULONG) c->Id()
                    << GPOS_WSZ_LIT(" name=") << c->Name().Pstr()->GetBuffer()
                    << GPOS_WSZ_LIT(" usage=") << UsageToSz(c->GetUsage())
                    << GPOS_WSZ_LIT(" attrnum=")
                    << (pcd ? (INT) pcd->AttrNum() : -999)
                    << GPOS_WSZ_LIT(" system=") << (is_sys ? 1 : 0)
                    << GPOS_WSZ_LIT(" dist=") << (is_dist ? 1 : 0)
                    << std::endl;

                // 可选：简单启发式告警（如果后续你同步 required→usage，可用额外信息替换）
                if (c->GetUsage() != CColRef::EUsed && !is_sys && !is_dist)
                {
                    // 标记潜在后续被“错误裁剪”的候选
                    at.Os() << GPOS_WSZ_LIT("[WRV-DYN-SCAN] NOTE possible-unused-output colid=")
                            << (ULONG) c->Id()
                            << GPOS_WSZ_LIT(" (may be pruned later unless rescued)") 
                            << std::endl;
                }
            }
        }

        // 打印分区信息
        at.Os() << GPOS_WSZ_LIT("[WRV-DYN-SCAN] partition_mdids count=")
                << (ULONG) m_partition_mdids->Size() << std::endl;

        // 映射逻辑：求出 partition mdid 在 root 所有子分区数组中的位置
        CMDAccessor *mda_local = COptCtxt::PoctxtFromTLS()->Pmda();
        const IMDRelation *root_rel = mda_local->RetrieveRel(ptabdesc->MDId());
        IMdIdArray *all_partition_mdids = root_rel->ChildPartitionMdids();

        for (ULONG ul = 0; ul < partition_mdids->Size(); ++ul)
        {
            IMDId *part_mdid = (*partition_mdids)[ul];
            ULONG pos = gpos::ulong_max;
            for (ULONG j = 0; j < all_partition_mdids->Size(); ++j)
            {
                if ((*all_partition_mdids)[j]->Equals(part_mdid))
                {
                    pos = j;
                    break;
                }
            }
            at.Os() << GPOS_WSZ_LIT("[WRV-DYN-SCAN] part_idx=") << (ULONG) ul
                    << GPOS_WSZ_LIT(" mdid=")
                    << CMDIdGPDB::CastMdid(part_mdid)->Oid()
                    << GPOS_WSZ_LIT(" global_pos=") << (pos == gpos::ulong_max ? -1 : (INT) pos)
                    << std::endl;
        }
    }

	GPOS_ASSERT(nullptr != pdrgpdrgpcrParts);
	GPOS_ASSERT(0 < pdrgpdrgpcrParts->Size());

	CMDAccessor *mda = COptCtxt::PoctxtFromTLS()->Pmda();
	const IMDRelation *root_rel = mda->RetrieveRel(ptabdesc->MDId());
	IMdIdArray *all_partition_mdids = root_rel->ChildPartitionMdids();
	ULONG part_ptr = 0;
	for (ULONG ul = 0; ul < partition_mdids->Size(); ul++)
	{
		IMDId *part_mdid = (*partition_mdids)[ul];
		while (part_mdid != (*all_partition_mdids)[part_ptr])
		{
			part_ptr++;
		}
		COptCtxt::PoctxtFromTLS()->AddPartForScanId(scan_id, part_ptr);
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalDynamicScan::~CPhysicalDynamicScan
//
//	@doc:
//		Dtor
//
//---------------------------------------------------------------------------
CPhysicalDynamicScan::~CPhysicalDynamicScan()
{
	m_pdrgpdrgpcrPart->Release();
	m_partition_mdids->Release();
	m_root_col_mapping_per_part->Release();
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalDynamicScan::HashValue
//
//	@doc:
//		Combine part index, pointer for table descriptor, Eop and output columns
//
//---------------------------------------------------------------------------
ULONG
CPhysicalDynamicScan::HashValue() const
{
	ULONG ulHash = gpos::CombineHashes(
		COperator::HashValue(),
		gpos::CombineHashes(gpos::HashValue(&m_scan_id),
							m_ptabdesc->MDId()->HashValue()));
	ulHash =
		gpos::CombineHashes(ulHash, CUtils::UlHashColArray(m_pdrgpcrOutput));

	return ulHash;
}

//---------------------------------------------------------------------------
//	@function:
//		CPhysicalDynamicScan::OsPrint
//
//	@doc:
//		debug print
//
//---------------------------------------------------------------------------
IOstream &
CPhysicalDynamicScan::OsPrint(IOstream &os) const
{
	os << SzId() << " ";

	// alias of table as referenced in the query
	m_pnameAlias->OsPrint(os);

	// actual name of table in catalog and columns
	os << " (";
	m_ptabdesc->Name().OsPrint(os);
	os << "), Columns: [";
	CUtils::OsPrintDrgPcr(os, m_pdrgpcrOutput);
	os << "] Scan Id: " << m_scan_id;
	os << " Parts to scan: " << m_partition_mdids->Size();


	return os;
}


//---------------------------------------------------------------------------
//	@function:
//		CPhysicalDynamicScan::PopConvert
//
//	@doc:
//		conversion function
//
//---------------------------------------------------------------------------
CPhysicalDynamicScan *
CPhysicalDynamicScan::PopConvert(COperator *pop)
{
	GPOS_ASSERT(nullptr != pop);
	GPOS_ASSERT(CUtils::FPhysicalScan(pop) &&
				CPhysicalScan::PopConvert(pop)->FDynamicScan());

	return dynamic_cast<CPhysicalDynamicScan *>(pop);
}


// EOF
