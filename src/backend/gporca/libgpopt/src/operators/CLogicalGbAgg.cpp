//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2009 Greenplum, Inc.
//
//	@filename:
//		CLogicalGbAgg.cpp
//
//	@doc:
//		Implementation of aggregate operator
//---------------------------------------------------------------------------

#include "gpopt/operators/CLogicalGbAgg.h"

#include "gpos/base.h"

#include "gpopt/base/CColRefSet.h"
#include "gpopt/base/CDrvdPropScalar.h"
#include "gpopt/base/CKeyCollection.h"
#include "gpopt/base/COptCtxt.h"
#include "gpopt/base/CUtils.h"
#include "gpopt/operators/CExpression.h"
#include "gpopt/operators/CExpressionHandle.h"
#include "naucrates/statistics/CGroupByStatsProcessor.h"

#define WRV_DEBUG_GBAGG
#ifdef WRV_DEBUG_GBAGG
#include "gpos/error/CAutoTrace.h"
#include "naucrates/md/IMDType.h"
#endif

using namespace gpopt;

#ifdef WRV_DEBUG_GBAGG
// 将 usage 枚举转为字符串
static const WCHAR *
WrvUsageToSz(CColRef::EUsedStatus us)
{
	switch (us)
	{
		case CColRef::EUsed: return GPOS_WSZ_LIT("EUsed");
		case CColRef::EUnused: return GPOS_WSZ_LIT("EUnused");
		case CColRef::EUnknown: return GPOS_WSZ_LIT("EUnknown");
		case CColRef::ESentinel: return GPOS_WSZ_LIT("ESentinel");
	}
	return GPOS_WSZ_LIT("E<?>");
}

// 打印一次分组/最小分组/DQA 信息
static void
WrvDumpGbAggInitial(const WCHAR *tag,
					CLogicalGbAgg *op,
					CColRefArray *pdrgpcrGrp,
					CColRefArray *pdrgpcrMinimal,
					CColRefArray *pdrgpcrDQA)
{
	CAutoTrace at(COptCtxt::PoctxtFromTLS()->Pmp());
	at.Os() << GPOS_WSZ_LIT("[WRV-GBAGG] ") << tag
			<< GPOS_WSZ_LIT(" grp_cols=")
			<< (ULONG)(pdrgpcrGrp ? pdrgpcrGrp->Size() : 0)
			<< GPOS_WSZ_LIT(" minimal_cols=")
			<< (ULONG)(pdrgpcrMinimal ? pdrgpcrMinimal->Size() : 0)
			<< GPOS_WSZ_LIT(" dqa_arg_cols=")
			<< (ULONG)(pdrgpcrDQA ? pdrgpcrDQA->Size() : 0)
			<< GPOS_WSZ_LIT(" agg_type=") << (INT) op->Egbaggtype()
			<< GPOS_WSZ_LIT(" agg_stage=") << (INT) op->AggStage()
			<< GPOS_WSZ_LIT(" generates_dup=")
			<< (op->FGeneratesDuplicates() ? 1 : 0)
			<< std::endl;

	if (pdrgpcrGrp)
	{
		for (ULONG i=0; i<pdrgpcrGrp->Size(); ++i)
		{
			CColRef *c = (*pdrgpcrGrp)[i];
			at.Os() << GPOS_WSZ_LIT("[WRV-GBAGG] GROUP idx=") << i
					<< GPOS_WSZ_LIT(" colid=") << c->Id()
					<< GPOS_WSZ_LIT(" name=") << c->Name().Pstr()->GetBuffer()
					<< GPOS_WSZ_LIT(" usage=") << WrvUsageToSz(c->GetUsage())
					<< GPOS_WSZ_LIT(" system=") << (c->IsSystemCol() ? 1 : 0)
					<< GPOS_WSZ_LIT(" dist=") << (c->IsDistCol() ? 1 : 0)
					<< GPOS_WSZ_LIT(" type_oid=")
					<< CMDIdGPDB::CastMdid(c->RetrieveType()->MDId())->Oid()
					<< std::endl;
		}
	}
	if (pdrgpcrMinimal && pdrgpcrMinimal != pdrgpcrGrp)
	{
		for (ULONG i=0; i<pdrgpcrMinimal->Size(); ++i)
		{
			CColRef *c = (*pdrgpcrMinimal)[i];
			at.Os() << GPOS_WSZ_LIT("[WRV-GBAGG] MINIMAL idx=") << i
					<< GPOS_WSZ_LIT(" colid=") << c->Id()
					<< GPOS_WSZ_LIT(" name=") << c->Name().Pstr()->GetBuffer()
					<< GPOS_WSZ_LIT(" usage=") << WrvUsageToSz(c->GetUsage())
					<< std::endl;
		}
	}
	if (pdrgpcrDQA)
	{
		for (ULONG i=0; i<pdrgpcrDQA->Size(); ++i)
		{
			CColRef *c = (*pdrgpcrDQA)[i];
			at.Os() << GPOS_WSZ_LIT("[WRV-GBAGG] DQA_ARG idx=") << i
					<< GPOS_WSZ_LIT(" colid=") << c->Id()
					<< GPOS_WSZ_LIT(" name=") << c->Name().Pstr()->GetBuffer()
					<< GPOS_WSZ_LIT(" usage=") << WrvUsageToSz(c->GetUsage())
					<< std::endl;
		}
	}
}
#endif // WRV_DEBUG_GBAGG


// 原 pattern ctor
CLogicalGbAgg::CLogicalGbAgg(CMemoryPool *mp)
	: CLogicalUnary(mp),
	  m_fGeneratesDuplicates(true),
	  m_pdrgpcrArgDQA(nullptr),
	  m_pdrgpcr(nullptr),
	  m_pdrgpcrMinimal(nullptr),
	  m_egbaggtype(COperator::EgbaggtypeSentinel),
	  m_aggStage(EasOthers)
{
	m_fPattern = true;
}
#include <execinfo.h>
static void WrvPrintStack(IOstream &os)
{
    void *addrs[64];
    int n = backtrace(addrs, 64);
    char **syms = backtrace_symbols(addrs, n);
    if (syms)
    {
        for (int i = 0; i < n; ++i)
        {
            os << "[WRV-GBAGG-STACK] " << syms[i] << std::endl;
        }
        free(syms);
    }
}
CLogicalGbAgg::CLogicalGbAgg(CMemoryPool *mp, CColRefArray *colref_array,
							 COperator::EGbAggType egbaggtype)
	: CLogicalUnary(mp),
	  m_fGeneratesDuplicates(false),
	  m_pdrgpcrArgDQA(nullptr),
	  m_pdrgpcr(colref_array),
	  m_pdrgpcrMinimal(nullptr),
	  m_egbaggtype(egbaggtype),
	  m_aggStage(EasOthers)
{
	if (COperator::EgbaggtypeLocal == egbaggtype)
	{
		m_fGeneratesDuplicates = true;
	}
	GPOS_ASSERT(nullptr != colref_array);
	GPOS_ASSERT(COperator::EgbaggtypeSentinel > egbaggtype);
	GPOS_ASSERT(COperator::EgbaggtypeIntermediate != egbaggtype);
	m_pcrsLocalUsed->Include(m_pdrgpcr);

#ifdef WRV_DEBUG_GBAGG
	WrvDumpGbAggInitial(GPOS_WSZ_LIT("CTOR1"), this, m_pdrgpcr, m_pdrgpcrMinimal, m_pdrgpcrArgDQA);
	if (m_pdrgpcr && m_pdrgpcr->Size() == 4)
{
    CAutoTrace at(mp);
    at.Os() << "[WRV-GBAGG-STACK] ctor5_size4 callstack BEGIN" << std::endl;
    WrvPrintStack(at.Os());
    at.Os() << "[WRV-GBAGG-STACK] ctor5_size4 callstack END" << std::endl;
}
#endif
}

CLogicalGbAgg::CLogicalGbAgg(CMemoryPool *mp, CColRefArray *colref_array,
							 COperator::EGbAggType egbaggtype,
							 EAggStage aggStage)
	: CLogicalUnary(mp),
	  m_fGeneratesDuplicates(false),
	  m_pdrgpcrArgDQA(nullptr),
	  m_pdrgpcr(colref_array),
	  m_pdrgpcrMinimal(nullptr),
	  m_egbaggtype(egbaggtype),
	  m_aggStage(aggStage)
{
	if (COperator::EgbaggtypeLocal == egbaggtype)
	{
		m_fGeneratesDuplicates = true;
	}
	GPOS_ASSERT(nullptr != colref_array);
	GPOS_ASSERT(COperator::EgbaggtypeSentinel > egbaggtype);
	GPOS_ASSERT(COperator::EgbaggtypeIntermediate != egbaggtype);
	m_pcrsLocalUsed->Include(m_pdrgpcr);

#ifdef WRV_DEBUG_GBAGG
	WrvDumpGbAggInitial(GPOS_WSZ_LIT("CTOR2"), this, m_pdrgpcr, m_pdrgpcrMinimal, m_pdrgpcrArgDQA);
#endif
}

CLogicalGbAgg::CLogicalGbAgg(CMemoryPool *mp, CColRefArray *colref_array,
							 COperator::EGbAggType egbaggtype,
							 BOOL fGeneratesDuplicates,
							 CColRefArray *pdrgpcrArgDQA)
	: CLogicalUnary(mp),
	  m_fGeneratesDuplicates(fGeneratesDuplicates),
	  m_pdrgpcrArgDQA(pdrgpcrArgDQA),
	  m_pdrgpcr(colref_array),
	  m_pdrgpcrMinimal(nullptr),
	  m_egbaggtype(egbaggtype),
	  m_aggStage(EasOthers)
{
	GPOS_ASSERT(nullptr != colref_array);
	GPOS_ASSERT(COperator::EgbaggtypeSentinel > egbaggtype);
	GPOS_ASSERT_IMP(nullptr == m_pdrgpcrArgDQA,
					COperator::EgbaggtypeIntermediate != egbaggtype);
	GPOS_ASSERT_IMP(m_fGeneratesDuplicates,
					COperator::EgbaggtypeLocal == egbaggtype);
	m_pcrsLocalUsed->Include(m_pdrgpcr);

#ifdef WRV_DEBUG_GBAGG
	WrvDumpGbAggInitial(GPOS_WSZ_LIT("CTOR3"), this, m_pdrgpcr, m_pdrgpcrMinimal, m_pdrgpcrArgDQA);
#endif
}

CLogicalGbAgg::CLogicalGbAgg(CMemoryPool *mp, CColRefArray *colref_array,
							 COperator::EGbAggType egbaggtype,
							 BOOL fGeneratesDuplicates,
							 CColRefArray *pdrgpcrArgDQA, EAggStage aggStage)
	: CLogicalUnary(mp),
	  m_fGeneratesDuplicates(fGeneratesDuplicates),
	  m_pdrgpcrArgDQA(pdrgpcrArgDQA),
	  m_pdrgpcr(colref_array),
	  m_pdrgpcrMinimal(nullptr),
	  m_egbaggtype(egbaggtype),
	  m_aggStage(aggStage)
{
	GPOS_ASSERT(nullptr != colref_array);
	GPOS_ASSERT(COperator::EgbaggtypeSentinel > egbaggtype);
	GPOS_ASSERT_IMP(nullptr == m_pdrgpcrArgDQA,
					COperator::EgbaggtypeIntermediate != egbaggtype);
	GPOS_ASSERT_IMP(m_fGeneratesDuplicates,
					COperator::EgbaggtypeLocal == egbaggtype);
	m_pcrsLocalUsed->Include(m_pdrgpcr);

#ifdef WRV_DEBUG_GBAGG
	WrvDumpGbAggInitial(GPOS_WSZ_LIT("CTOR4"), this, m_pdrgpcr, m_pdrgpcrMinimal, m_pdrgpcrArgDQA);
#endif
}

CLogicalGbAgg::CLogicalGbAgg(CMemoryPool *mp, CColRefArray *colref_array,
							 CColRefArray *pdrgpcrMinimal,
							 COperator::EGbAggType egbaggtype)
	: CLogicalUnary(mp),
	  m_fGeneratesDuplicates(true),
	  m_pdrgpcrArgDQA(nullptr),
	  m_pdrgpcr(colref_array),
	  m_pdrgpcrMinimal(pdrgpcrMinimal),
	  m_egbaggtype(egbaggtype),
	  m_aggStage(EasOthers)
{
	GPOS_ASSERT(nullptr != colref_array);
	GPOS_ASSERT(COperator::EgbaggtypeSentinel > egbaggtype);
	GPOS_ASSERT(COperator::EgbaggtypeIntermediate != egbaggtype);
	GPOS_ASSERT_IMP(nullptr != pdrgpcrMinimal,
					pdrgpcrMinimal->Size() <= colref_array->Size());

	if (nullptr == pdrgpcrMinimal)
	{
		m_pdrgpcr->AddRef();
		m_pdrgpcrMinimal = m_pdrgpcr;
	}
	m_pcrsLocalUsed->Include(m_pdrgpcr);

#ifdef WRV_DEBUG_GBAGG
	WrvDumpGbAggInitial(GPOS_WSZ_LIT("CTOR5"), this, m_pdrgpcr, m_pdrgpcrMinimal, m_pdrgpcrArgDQA);
#endif
// 在 CTOR5 构造体里（grp_cols=4 那个版本之后）：
#ifdef WRV_DEBUG_GBAGG
if (m_pdrgpcr && m_pdrgpcr->Size() == 4)
{
    CAutoTrace at(mp);
    at.Os() << "[WRV-GBAGG-STACK] ctor5_size4 callstack BEGIN" << std::endl;
    WrvPrintStack(at.Os());
    at.Os() << "[WRV-GBAGG-STACK] ctor5_size4 callstack END" << std::endl;
}
#endif
}

CLogicalGbAgg::CLogicalGbAgg(CMemoryPool *mp, CColRefArray *colref_array,
							 CColRefArray *pdrgpcrMinimal,
							 COperator::EGbAggType egbaggtype,
							 BOOL fGeneratesDuplicates,
							 CColRefArray *pdrgpcrArgDQA)
	: CLogicalUnary(mp),
	  m_fGeneratesDuplicates(fGeneratesDuplicates),
	  m_pdrgpcrArgDQA(pdrgpcrArgDQA),
	  m_pdrgpcr(colref_array),
	  m_pdrgpcrMinimal(pdrgpcrMinimal),
	  m_egbaggtype(egbaggtype),
	  m_aggStage(EasOthers)
{
	GPOS_ASSERT(nullptr != colref_array);
	GPOS_ASSERT(COperator::EgbaggtypeSentinel > egbaggtype);
	GPOS_ASSERT_IMP(nullptr != pdrgpcrMinimal,
					pdrgpcrMinimal->Size() <= colref_array->Size());
	GPOS_ASSERT_IMP(nullptr == m_pdrgpcrArgDQA,
					COperator::EgbaggtypeIntermediate != egbaggtype);
	GPOS_ASSERT_IMP(m_fGeneratesDuplicates,
					COperator::EgbaggtypeLocal == egbaggtype);

	if (nullptr == pdrgpcrMinimal)
	{
		m_pdrgpcr->AddRef();
		m_pdrgpcrMinimal = m_pdrgpcr;
	}
	m_pcrsLocalUsed->Include(m_pdrgpcr);

#ifdef WRV_DEBUG_GBAGG
	WrvDumpGbAggInitial(GPOS_WSZ_LIT("CTOR6"), this, m_pdrgpcr, m_pdrgpcrMinimal, m_pdrgpcrArgDQA);
#endif
}

// ------------------ 下面保持原有函数不变，只在 DeriveOutputColumns 末尾加调试 ------------------

CLogicalGbAgg::~CLogicalGbAgg()
{
	CRefCount::SafeRelease(m_pdrgpcr);
	CRefCount::SafeRelease(m_pdrgpcrMinimal);
	CRefCount::SafeRelease(m_pdrgpcrArgDQA);
}

COperator *
CLogicalGbAgg::PopCopyWithRemappedColumns(CMemoryPool *mp,
										  UlongToColRefMap *colref_mapping,
										  BOOL must_exist)
{
	// 原逻辑不改
	CColRefArray *colref_array =
		CUtils::PdrgpcrRemap(mp, m_pdrgpcr, colref_mapping, must_exist);
	CColRefArray *pdrgpcrMinimal = nullptr;
	if (nullptr != m_pdrgpcrMinimal)
	{
		pdrgpcrMinimal = CUtils::PdrgpcrRemap(
			mp, m_pdrgpcrMinimal, colref_mapping, must_exist);
	}
	CColRefArray *pdrgpcrArgDQA = nullptr;
	if (nullptr != m_pdrgpcrArgDQA)
	{
		pdrgpcrArgDQA = CUtils::PdrgpcrRemap(
			mp, m_pdrgpcrArgDQA, colref_mapping, must_exist);
	}

	return GPOS_NEW(mp)
		CLogicalGbAgg(mp, colref_array, pdrgpcrMinimal, Egbaggtype(),
					  m_fGeneratesDuplicates, pdrgpcrArgDQA);
}

CColRefSet *
CLogicalGbAgg::DeriveOutputColumns(CMemoryPool *mp, CExpressionHandle &exprhdl)
{
	GPOS_ASSERT(2 == exprhdl.Arity());

	CColRefSet *pcrs = GPOS_NEW(mp) CColRefSet(mp);
	pcrs->Include(Pdrgpcr());
	pcrs->Intersection(exprhdl.DeriveOutputColumns(0));
	pcrs->Union(exprhdl.DeriveDefinedColumns(1));

#ifdef WRV_DEBUG_GBAGG
	{
		CAutoTrace at(mp);
		at.Os() << GPOS_WSZ_LIT("[WRV-GBAGG] DeriveOutputColumns grp_cols=")
				<< (ULONG)m_pdrgpcr->Size()
				<< GPOS_WSZ_LIT(" final_output_cols=")
				<< (ULONG)pcrs->Size()
				<< std::endl;

		// 如果分组列数 == 输出列数，可标个提示（可能是 broad grouping）
		if (pcrs->Size() == m_pdrgpcr->Size() && pcrs->Size() > 0)
		{
			at.Os() << GPOS_WSZ_LIT("[WRV-GBAGG] NOTE output == group_cols (check if whole-row or over-grouping)")
					<< std::endl;
		}
	}
#endif
	return pcrs;
}
#if 0
//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::CLogicalGbAgg
//
//	@doc:
//		ctor for xform pattern
//
//---------------------------------------------------------------------------
CLogicalGbAgg::CLogicalGbAgg(CMemoryPool *mp)
	: CLogicalUnary(mp),
	  m_fGeneratesDuplicates(true),
	  m_pdrgpcrArgDQA(nullptr),
	  m_pdrgpcr(nullptr),
	  m_pdrgpcrMinimal(nullptr),
	  m_egbaggtype(COperator::EgbaggtypeSentinel),
	  m_aggStage(EasOthers)
{
	m_fPattern = true;
}


//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::CLogicalGbAgg
//
//	@doc:
//		ctor
//
//---------------------------------------------------------------------------
CLogicalGbAgg::CLogicalGbAgg(CMemoryPool *mp, CColRefArray *colref_array,
							 COperator::EGbAggType egbaggtype)
	: CLogicalUnary(mp),
	  m_fGeneratesDuplicates(false),
	  m_pdrgpcrArgDQA(nullptr),
	  m_pdrgpcr(colref_array),
	  m_pdrgpcrMinimal(nullptr),
	  m_egbaggtype(egbaggtype),
	  m_aggStage(EasOthers)
{
	if (COperator::EgbaggtypeLocal == egbaggtype)
	{
		// final and intermediate aggregates have to remove duplicates for a given group
		m_fGeneratesDuplicates = true;
	}

	GPOS_ASSERT(nullptr != colref_array);
	GPOS_ASSERT(COperator::EgbaggtypeSentinel > egbaggtype);
	GPOS_ASSERT(COperator::EgbaggtypeIntermediate != egbaggtype);

	m_pcrsLocalUsed->Include(m_pdrgpcr);
}

CLogicalGbAgg::CLogicalGbAgg(CMemoryPool *mp, CColRefArray *colref_array,
							 COperator::EGbAggType egbaggtype,
							 EAggStage aggStage)
	: CLogicalUnary(mp),
	  m_fGeneratesDuplicates(false),
	  m_pdrgpcrArgDQA(nullptr),
	  m_pdrgpcr(colref_array),
	  m_pdrgpcrMinimal(nullptr),
	  m_egbaggtype(egbaggtype),
	  m_aggStage(aggStage)
{
	if (COperator::EgbaggtypeLocal == egbaggtype)
	{
		// final and intermediate aggregates have to remove duplicates for a given group
		m_fGeneratesDuplicates = true;
	}

	GPOS_ASSERT(nullptr != colref_array);
	GPOS_ASSERT(COperator::EgbaggtypeSentinel > egbaggtype);
	GPOS_ASSERT(COperator::EgbaggtypeIntermediate != egbaggtype);

	m_pcrsLocalUsed->Include(m_pdrgpcr);
}


//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::CLogicalGbAgg
//
//	@doc:
//		ctor
//
//---------------------------------------------------------------------------
CLogicalGbAgg::CLogicalGbAgg(CMemoryPool *mp, CColRefArray *colref_array,
							 COperator::EGbAggType egbaggtype,
							 BOOL fGeneratesDuplicates,
							 CColRefArray *pdrgpcrArgDQA)
	: CLogicalUnary(mp),
	  m_fGeneratesDuplicates(fGeneratesDuplicates),
	  m_pdrgpcrArgDQA(pdrgpcrArgDQA),
	  m_pdrgpcr(colref_array),
	  m_pdrgpcrMinimal(nullptr),
	  m_egbaggtype(egbaggtype),
	  m_aggStage(EasOthers)
{
	GPOS_ASSERT(nullptr != colref_array);
	GPOS_ASSERT(COperator::EgbaggtypeSentinel > egbaggtype);
	GPOS_ASSERT_IMP(nullptr == m_pdrgpcrArgDQA,
					COperator::EgbaggtypeIntermediate != egbaggtype);
	GPOS_ASSERT_IMP(m_fGeneratesDuplicates,
					COperator::EgbaggtypeLocal == egbaggtype);

	m_pcrsLocalUsed->Include(m_pdrgpcr);
}

CLogicalGbAgg::CLogicalGbAgg(CMemoryPool *mp, CColRefArray *colref_array,
							 COperator::EGbAggType egbaggtype,
							 BOOL fGeneratesDuplicates,
							 CColRefArray *pdrgpcrArgDQA, EAggStage aggStage)
	: CLogicalUnary(mp),
	  m_fGeneratesDuplicates(fGeneratesDuplicates),
	  m_pdrgpcrArgDQA(pdrgpcrArgDQA),
	  m_pdrgpcr(colref_array),
	  m_pdrgpcrMinimal(nullptr),
	  m_egbaggtype(egbaggtype),
	  m_aggStage(aggStage)
{
	GPOS_ASSERT(nullptr != colref_array);
	GPOS_ASSERT(COperator::EgbaggtypeSentinel > egbaggtype);
	GPOS_ASSERT_IMP(nullptr == m_pdrgpcrArgDQA,
					COperator::EgbaggtypeIntermediate != egbaggtype);
	GPOS_ASSERT_IMP(m_fGeneratesDuplicates,
					COperator::EgbaggtypeLocal == egbaggtype);

	m_pcrsLocalUsed->Include(m_pdrgpcr);
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::CLogicalGbAgg
//
//	@doc:
//		ctor
//
//---------------------------------------------------------------------------
CLogicalGbAgg::CLogicalGbAgg(CMemoryPool *mp, CColRefArray *colref_array,
							 CColRefArray *pdrgpcrMinimal,
							 COperator::EGbAggType egbaggtype)
	: CLogicalUnary(mp),
	  m_fGeneratesDuplicates(true),
	  m_pdrgpcrArgDQA(nullptr),
	  m_pdrgpcr(colref_array),
	  m_pdrgpcrMinimal(pdrgpcrMinimal),
	  m_egbaggtype(egbaggtype),
	  m_aggStage(EasOthers)
{
	GPOS_ASSERT(nullptr != colref_array);
	GPOS_ASSERT(COperator::EgbaggtypeSentinel > egbaggtype);
	GPOS_ASSERT(COperator::EgbaggtypeIntermediate != egbaggtype);

	GPOS_ASSERT_IMP(nullptr != pdrgpcrMinimal,
					pdrgpcrMinimal->Size() <= colref_array->Size());

	if (nullptr == pdrgpcrMinimal)
	{
		m_pdrgpcr->AddRef();
		m_pdrgpcrMinimal = m_pdrgpcr;
	}

	m_pcrsLocalUsed->Include(m_pdrgpcr);
}


//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::CLogicalGbAgg
//
//	@doc:
//		ctor
//
//---------------------------------------------------------------------------
CLogicalGbAgg::CLogicalGbAgg(CMemoryPool *mp, CColRefArray *colref_array,
							 CColRefArray *pdrgpcrMinimal,
							 COperator::EGbAggType egbaggtype,
							 BOOL fGeneratesDuplicates,
							 CColRefArray *pdrgpcrArgDQA)
	: CLogicalUnary(mp),
	  m_fGeneratesDuplicates(fGeneratesDuplicates),
	  m_pdrgpcrArgDQA(pdrgpcrArgDQA),
	  m_pdrgpcr(colref_array),
	  m_pdrgpcrMinimal(pdrgpcrMinimal),
	  m_egbaggtype(egbaggtype),
	  m_aggStage(EasOthers)
{
	GPOS_ASSERT(nullptr != colref_array);
	GPOS_ASSERT(COperator::EgbaggtypeSentinel > egbaggtype);

	GPOS_ASSERT_IMP(nullptr != pdrgpcrMinimal,
					pdrgpcrMinimal->Size() <= colref_array->Size());
	GPOS_ASSERT_IMP(nullptr == m_pdrgpcrArgDQA,
					COperator::EgbaggtypeIntermediate != egbaggtype);
	GPOS_ASSERT_IMP(m_fGeneratesDuplicates,
					COperator::EgbaggtypeLocal == egbaggtype);

	if (nullptr == pdrgpcrMinimal)
	{
		m_pdrgpcr->AddRef();
		m_pdrgpcrMinimal = m_pdrgpcr;
	}

	m_pcrsLocalUsed->Include(m_pdrgpcr);
}


//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::~CLogicalGbAgg
//
//	@doc:
//		dtor
//
//---------------------------------------------------------------------------
CLogicalGbAgg::~CLogicalGbAgg()
{
	// safe release -- to allow for instances used in patterns
	CRefCount::SafeRelease(m_pdrgpcr);
	CRefCount::SafeRelease(m_pdrgpcrMinimal);
	CRefCount::SafeRelease(m_pdrgpcrArgDQA);
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::PopCopyWithRemappedColumns
//
//	@doc:
//		Return a copy of the operator with remapped columns
//
//---------------------------------------------------------------------------
COperator *
CLogicalGbAgg::PopCopyWithRemappedColumns(CMemoryPool *mp,
										  UlongToColRefMap *colref_mapping,
										  BOOL must_exist)
{
	CColRefArray *colref_array =
		CUtils::PdrgpcrRemap(mp, m_pdrgpcr, colref_mapping, must_exist);
	CColRefArray *pdrgpcrMinimal = nullptr;
	if (nullptr != m_pdrgpcrMinimal)
	{
		pdrgpcrMinimal = CUtils::PdrgpcrRemap(mp, m_pdrgpcrMinimal,
											  colref_mapping, must_exist);
	}

	CColRefArray *pdrgpcrArgDQA = nullptr;
	if (nullptr != m_pdrgpcrArgDQA)
	{
		pdrgpcrArgDQA = CUtils::PdrgpcrRemap(mp, m_pdrgpcrArgDQA,
											 colref_mapping, must_exist);
	}

	return GPOS_NEW(mp)
		CLogicalGbAgg(mp, colref_array, pdrgpcrMinimal, Egbaggtype(),
					  m_fGeneratesDuplicates, pdrgpcrArgDQA);
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::DeriveOutputColumns
//
//	@doc:
//		Derive output columns
//
//---------------------------------------------------------------------------
CColRefSet *
CLogicalGbAgg::DeriveOutputColumns(CMemoryPool *mp, CExpressionHandle &exprhdl)
{
	GPOS_ASSERT(2 == exprhdl.Arity());

	CColRefSet *pcrs = GPOS_NEW(mp) CColRefSet(mp);

	// include the intersection of the grouping columns and the child's output
	pcrs->Include(Pdrgpcr());
	pcrs->Intersection(exprhdl.DeriveOutputColumns(0));

	// the scalar child defines additional columns
	pcrs->Union(exprhdl.DeriveDefinedColumns(1));

	return pcrs;
}
#endif
//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::DeriveOuterReferences
//
//	@doc:
//		Derive outer references
//
//---------------------------------------------------------------------------
CColRefSet *
CLogicalGbAgg::DeriveOuterReferences(CMemoryPool *mp,
									 CExpressionHandle &exprhdl)
{
	CColRefSet *pcrsGrp = GPOS_NEW(mp) CColRefSet(mp);
	pcrsGrp->Include(m_pdrgpcr);

	CColRefSet *outer_refs =
		CLogical::DeriveOuterReferences(mp, exprhdl, pcrsGrp);
	pcrsGrp->Release();

	return outer_refs;
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::DerivePropertyConstraint
//
//	@doc:
//		Derive constraint property
//
//---------------------------------------------------------------------------
CPropConstraint *
CLogicalGbAgg::DerivePropertyConstraint(CMemoryPool *mp,
										CExpressionHandle &exprhdl) const
{
	CColRefSet *pcrsGrouping = GPOS_NEW(mp) CColRefSet(mp);
	pcrsGrouping->Include(m_pdrgpcr);

	// get the constraints on the grouping columns only
	CPropConstraint *ppc =
		PpcDeriveConstraintRestrict(mp, exprhdl, pcrsGrouping);
	pcrsGrouping->Release();

	return ppc;
}


//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::PcrsStat
//
//	@doc:
//		Compute required stats columns
//
//---------------------------------------------------------------------------
CColRefSet *
CLogicalGbAgg::PcrsStat(CMemoryPool *mp, CExpressionHandle &exprhdl,
						CColRefSet *pcrsInput, ULONG child_index) const
{
	return PcrsStatGbAgg(mp, exprhdl, pcrsInput, child_index, m_pdrgpcr);
}


//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::PcrsStatGbAgg
//
//	@doc:
//		Compute required stats columns for a GbAgg
//
//---------------------------------------------------------------------------
CColRefSet *
CLogicalGbAgg::PcrsStatGbAgg(CMemoryPool *mp, CExpressionHandle &exprhdl,
							 CColRefSet *pcrsInput, ULONG child_index,
							 CColRefArray *pdrgpcrGrp) const
{
	GPOS_ASSERT(nullptr != pdrgpcrGrp);
	CColRefSet *pcrs = GPOS_NEW(mp) CColRefSet(mp);

	// include grouping columns
	pcrs->Include(pdrgpcrGrp);

	// other columns used in aggregates
	pcrs->Union(exprhdl.DeriveUsedColumns(1));

	// if the grouping column is a computed column, then add its corresponding used columns
	// to required columns for statistics computation
	CColumnFactory *col_factory = COptCtxt::PoctxtFromTLS()->Pcf();
	const ULONG ulGrpCols = m_pdrgpcr->Size();
	for (ULONG ul = 0; ul < ulGrpCols; ul++)
	{
		CColRef *pcrGrpCol = (*m_pdrgpcr)[ul];
		const CColRefSet *pcrsUsed =
			col_factory->PcrsUsedInComputedCol(pcrGrpCol);
		if (nullptr != pcrsUsed)
		{
			pcrs->Union(pcrsUsed);
		}
	}

	CColRefSet *pcrsRequired =
		PcrsReqdChildStats(mp, exprhdl, pcrsInput, pcrs, child_index);
	pcrs->Release();

	return pcrsRequired;
}


//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::DeriveNotNullColumns
//
//	@doc:
//		Derive not null columns
//
//---------------------------------------------------------------------------
CColRefSet *
CLogicalGbAgg::DeriveNotNullColumns(CMemoryPool *mp,
									CExpressionHandle &exprhdl) const
{
	GPOS_ASSERT(2 == exprhdl.Arity());

	CColRefSet *pcrs = GPOS_NEW(mp) CColRefSet(mp);

	// include grouping columns
	pcrs->Include(Pdrgpcr());

	// intersect with not nullable columns from relational child
	pcrs->Intersection(exprhdl.DeriveNotNullColumns(0));

	// TODO,  03/18/2012, add nullability info of computed columns

	return pcrs;
}


//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::HashValue
//
//	@doc:
//		Operator specific hash function
//
//---------------------------------------------------------------------------
ULONG
CLogicalGbAgg::HashValue() const
{
	ULONG ulHash = COperator::HashValue();
	ULONG arity = m_pdrgpcr->Size();
	ULONG ulGbaggtype = (ULONG) m_egbaggtype;

	for (ULONG ul = 0; ul < arity; ul++)
	{
		CColRef *colref = (*m_pdrgpcr)[ul];
		ulHash = gpos::CombineHashes(ulHash, gpos::HashPtr<CColRef>(colref));
	}

	ulHash = gpos::CombineHashes(ulHash, gpos::HashValue<ULONG>(&ulGbaggtype));

	return gpos::CombineHashes(ulHash,
							   gpos::HashValue<BOOL>(&m_fGeneratesDuplicates));
}


//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::PkcDeriveKeys
//
//	@doc:
//		Derive key collection
//
//---------------------------------------------------------------------------
CKeyCollection *
CLogicalGbAgg::DeriveKeyCollection(CMemoryPool *mp,
								   CExpressionHandle &exprhdl) const
{
	CKeyCollection *pkc = nullptr;

	// Gb produces a key only if it's global
	if (FGlobal())
	{
		if (COperator::EgbaggtypeLocal == m_egbaggtype &&
			!m_fGeneratesDuplicates)
		{
			return pkc;
		}

		if (0 < m_pdrgpcr->Size())
		{
			// grouping columns always constitute a key
			m_pdrgpcr->AddRef();
			pkc = GPOS_NEW(mp) CKeyCollection(mp, m_pdrgpcr);
		}
		else
		{
			// scalar and single-group aggs produce one row that constitutes a key
			CColRefSet *pcrs = exprhdl.DeriveDefinedColumns(1);

			if (0 == pcrs->Size())
			{
				// aggregate defines no columns, e.g. select 1 from r group by a
				return nullptr;
			}

			pcrs->AddRef();
			pkc = GPOS_NEW(mp) CKeyCollection(mp, pcrs);
		}
	}

	return pkc;
}


//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::DeriveMaxCard
//
//	@doc:
//		Derive max card
//
//---------------------------------------------------------------------------
CMaxCard
CLogicalGbAgg::DeriveMaxCard(CMemoryPool *,	 //mp
							 CExpressionHandle &exprhdl) const
{
	// agg w/o grouping columns produces one row
	if (0 == m_pdrgpcr->Size())
	{
		return CMaxCard(1 /*ull*/);
	}

	// contradictions produce no rows
	if (exprhdl.DerivePropertyConstraint()->FContradiction())
	{
		return CMaxCard(0 /*ull*/);
	}

	return CMaxCard();
}


//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::Matches
//
//	@doc:
//		Match function on operator level
//
//---------------------------------------------------------------------------
BOOL
CLogicalGbAgg::Matches(COperator *pop) const
{
	if (pop->Eopid() != Eopid())
	{
		return false;
	}

	CLogicalGbAgg *popAgg = dynamic_cast<CLogicalGbAgg *>(pop);

	return FGeneratesDuplicates() == popAgg->FGeneratesDuplicates() &&
		   popAgg->Egbaggtype() == m_egbaggtype &&
		   CColRef::Equals(m_pdrgpcr, popAgg->m_pdrgpcr) &&
		   CColRef::Equals(m_pdrgpcrMinimal, popAgg->PdrgpcrMinimal()) &&
		   CColRef::Equals(m_pdrgpcrArgDQA, popAgg->PdrgpcrArgDQA());
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::PxfsCandidates
//
//	@doc:
//		Get candidate xforms
//
//---------------------------------------------------------------------------
CXformSet *
CLogicalGbAgg::PxfsCandidates(CMemoryPool *mp) const
{
	CXformSet *xform_set = GPOS_NEW(mp) CXformSet(mp);

	(void) xform_set->ExchangeSet(CXform::ExfSimplifyGbAgg);
	(void) xform_set->ExchangeSet(CXform::ExfGbAggWithMDQA2Join);
	(void) xform_set->ExchangeSet(CXform::ExfCollapseGbAgg);
	(void) xform_set->ExchangeSet(CXform::ExfPushGbBelowJoin);
	(void) xform_set->ExchangeSet(CXform::ExfPushGbBelowUnion);
	(void) xform_set->ExchangeSet(CXform::ExfPushGbBelowUnionAll);
	if (FGlobal())
	{
		(void) xform_set->ExchangeSet(CXform::ExfSplitGbAgg);
	}
	(void) xform_set->ExchangeSet(CXform::ExfSplitDQA);
	(void) xform_set->ExchangeSet(CXform::ExfGbAgg2Apply);
	(void) xform_set->ExchangeSet(CXform::ExfGbAgg2HashAgg);
	(void) xform_set->ExchangeSet(CXform::ExfGbAgg2StreamAgg);
	(void) xform_set->ExchangeSet(CXform::ExfGbAgg2ScalarAgg);
	(void) xform_set->ExchangeSet(CXform::ExfEagerAgg);
	(void) xform_set->ExchangeSet(CXform::ExfMinMax2IndexGet);
	(void) xform_set->ExchangeSet(CXform::ExfMinMax2IndexOnlyGet);
	return xform_set;
}


//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::PstatsDerive
//
//	@doc:
//		Derive statistics
//
//---------------------------------------------------------------------------
IStatistics *
CLogicalGbAgg::PstatsDerive(CMemoryPool *mp, IStatistics *child_stats,
							CColRefArray *pdrgpcrGroupingCols,
							ULongPtrArray *pdrgpulComputedCols, CBitSet *keys)
{
	const ULONG ulGroupingCols = pdrgpcrGroupingCols->Size();

	// extract grouping column ids
	ULongPtrArray *pdrgpulGroupingCols = GPOS_NEW(mp) ULongPtrArray(mp);
	for (ULONG ul = 0; ul < ulGroupingCols; ul++)
	{
		CColRef *colref = (*pdrgpcrGroupingCols)[ul];
		pdrgpulGroupingCols->Append(GPOS_NEW(mp) ULONG(colref->Id()));
	}

	IStatistics *stats = CGroupByStatsProcessor::CalcGroupByStats(
		mp, dynamic_cast<CStatistics *>(child_stats), pdrgpulGroupingCols,
		pdrgpulComputedCols, keys);

	// clean up
	pdrgpulGroupingCols->Release();

	return stats;
}


//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::PstatsDerive
//
//	@doc:
//		Derive statistics
//
//---------------------------------------------------------------------------
IStatistics *
CLogicalGbAgg::PstatsDerive(CMemoryPool *mp, CExpressionHandle &exprhdl,
							IStatisticsArray *	// not used
) const
{
	GPOS_ASSERT(Esp(exprhdl) > EspNone);
	IStatistics *child_stats = exprhdl.Pstats(0);

	// extract computed columns
	ULongPtrArray *pdrgpulComputedCols = GPOS_NEW(mp) ULongPtrArray(mp);
	exprhdl.DeriveDefinedColumns(1)->ExtractColIds(mp, pdrgpulComputedCols);

	IStatistics *stats = PstatsDerive(mp, child_stats, Pdrgpcr(),
									  pdrgpulComputedCols, nullptr /*keys*/);

	pdrgpulComputedCols->Release();

	return stats;
}

BOOL
CLogicalGbAgg::IsTwoStageScalarDQA() const
{
	return (m_aggStage == EasTwoStageScalarDQA);
}

BOOL
CLogicalGbAgg::IsThreeStageScalarDQA() const
{
	return (m_aggStage == EasThreeStageScalarDQA);
}

//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::OsPrint
//
//	@doc:
//		debug print
//
//---------------------------------------------------------------------------
IOstream &
CLogicalGbAgg::OsPrint(IOstream &os) const
{
	if (m_fPattern)
	{
		return COperator::OsPrint(os);
	}

	os << SzId() << "( ";
	OsPrintGbAggType(os, m_egbaggtype);
	os << " )";
	os << " Grp Cols: [";
	CUtils::OsPrintDrgPcr(os, m_pdrgpcr);
	os << "]"
	   << "[";
	OsPrintGbAggType(os, m_egbaggtype);
	os << "]";

	os << ", Minimal Grp Cols: [";
	if (nullptr != m_pdrgpcrMinimal)
	{
		CUtils::OsPrintDrgPcr(os, m_pdrgpcrMinimal);
	}
	os << "]";

	if (COperator::EgbaggtypeIntermediate == m_egbaggtype)
	{
		os << ", Distinct Cols:[";
		CUtils::OsPrintDrgPcr(os, m_pdrgpcrArgDQA);
		os << "]";
	}

	os << ", Generates Duplicates :[ " << FGeneratesDuplicates() << " ] ";

	if (IsTwoStageScalarDQA())
	{
		os << ", m_aggStage :[  Two Stage Scalar DQA  ] ";
	}

	if (IsThreeStageScalarDQA())
	{
		os << ", m_aggStage :[  Three Stage Scalar DQA  ] ";
	}

	return os;
}


//---------------------------------------------------------------------------
//	@function:
//		CLogicalGbAgg::OsPrintGbAggType
//
//	@doc:
//		Helper function to print aggregate type
//
//---------------------------------------------------------------------------
IOstream &
CLogicalGbAgg::OsPrintGbAggType(IOstream &os, COperator::EGbAggType egbaggtype)
{
	switch (egbaggtype)
	{
		case COperator::EgbaggtypeGlobal:
			os << "Global";
			break;

		case COperator::EgbaggtypeIntermediate:
			os << "Intermediate";
			break;

		case COperator::EgbaggtypeLocal:
			os << "Local";
			break;

		default:
			GPOS_ASSERT(!"Unsupported aggregate type");
	}
	return os;
}

// EOF
