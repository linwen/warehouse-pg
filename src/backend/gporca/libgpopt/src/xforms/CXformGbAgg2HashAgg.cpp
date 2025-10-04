//---------------------------------------------------------------------------
//	Greenplum Database
//	Copyright (C) 2009 Greenplum, Inc.
//
//	@filename:
//		CXformGbAgg2HashAgg.cpp
//
//	@doc:
//		Implementation of transform
//---------------------------------------------------------------------------

#include "gpopt/xforms/CXformGbAgg2HashAgg.h"

#include "gpos/base.h"

#include "gpopt/base/COptCtxt.h"
#include "gpopt/operators/CLogicalGbAgg.h"
#include "gpopt/operators/CPatternLeaf.h"
#include "gpopt/operators/CPhysicalHashAgg.h"
#include "gpopt/xforms/CXformUtils.h"
#include "naucrates/md/IMDAggregate.h"

using namespace gpopt;


//---------------------------------------------------------------------------
//	@function:
//		CXformGbAgg2HashAgg::CXformGbAgg2HashAgg
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CXformGbAgg2HashAgg::CXformGbAgg2HashAgg(CMemoryPool *mp)
	: CXformImplementation(
		  // pattern
		  GPOS_NEW(mp) CExpression(
			  mp, GPOS_NEW(mp) CLogicalGbAgg(mp),
			  GPOS_NEW(mp) CExpression(mp, GPOS_NEW(mp) CPatternLeaf(mp)),
			  // we need to extract deep tree in the project list to check
			  // for existence of distinct agg functions
			  GPOS_NEW(mp) CExpression(mp, GPOS_NEW(mp) CPatternTree(mp))))
{
}

//---------------------------------------------------------------------------
//	@function:
//		CXformGbAgg2HashAgg::CXformGbAgg2HashAgg
//
//	@doc:
//		Ctor
//
//---------------------------------------------------------------------------
CXformGbAgg2HashAgg::CXformGbAgg2HashAgg(CExpression *pexprPattern)
	: CXformImplementation(pexprPattern)
{
}

//---------------------------------------------------------------------------
//	@function:
//		CXformGbAgg2HashAgg::Exfp
//
//	@doc:
//		Compute xform promise for a given expression handle;
//		grouping columns must be non-empty
//
//---------------------------------------------------------------------------
CXform::EXformPromise
CXformGbAgg2HashAgg::Exfp(CExpressionHandle &exprhdl) const
{
	CLogicalGbAgg *popAgg = CLogicalGbAgg::PopConvert(exprhdl.Pop());
	CColRefArray *colref_array = popAgg->Pdrgpcr();
	if (0 == colref_array->Size() || exprhdl.DeriveHasSubquery(1) ||
		!CUtils::FComparisonPossible(colref_array, IMDType::EcmptEq) ||
		!CUtils::IsHashable(colref_array))
	{
		// no grouping columns, no equality or hash operators  are available for grouping columns, or
		// agg functions use subquery arguments
		return CXform::ExfpNone;
	}

	return CXform::ExfpHigh;
}

//---------------------------------------------------------------------------
//	@function:
//		CXformGbAgg2HashAgg::Transform
//
//	@doc:
//		Actual transformation
//
//---------------------------------------------------------------------------
void
CXformGbAgg2HashAgg::Transform(CXformContext *pxfctxt, CXformResult *pxfres,
							   CExpression *pexpr) const
{
	GPOS_ASSERT(nullptr != pxfctxt);
	GPOS_ASSERT(FPromising(pxfctxt->Pmp(), this, pexpr));
	GPOS_ASSERT(FCheckPattern(pexpr));

	// hash agg is not used with distinct agg functions
	// hash agg is not used if agg function does not have prelim func
	// hash agg is not used for ordered aggregate
	// evaluating these conditions needs a deep tree in the project list
	if (!FApplicable(pexpr))
	{
					CAutoTrace at(pxfctxt->Pmp());
			at.Os() << "[WRV-HASHAGG-XFORM] Skip (not applicable) expr_id="
					<< (ULONG_PTR) pexpr << std::endl;
					return;
	}

	CMemoryPool *mp = pxfctxt->Pmp();
	CLogicalGbAgg *popAgg = CLogicalGbAgg::PopConvert(pexpr->Pop());
	CColRefArray *colref_array = popAgg->Pdrgpcr();
	colref_array->AddRef();

	// extract components
	CExpression *pexprRel = (*pexpr)[0];
	CExpression *pexprScalar = (*pexpr)[1];

	// addref children
	pexprRel->AddRef();
	pexprScalar->AddRef();

	CColRefArray *pdrgpcrArgDQA = popAgg->PdrgpcrArgDQA();
	if (pdrgpcrArgDQA != nullptr && 0 != pdrgpcrArgDQA->Size())
	{
		GPOS_ASSERT(nullptr != pdrgpcrArgDQA);
		pdrgpcrArgDQA->AddRef();
	}
	auto UsageToStr = [](CColRef::EUsedStatus s) -> const WCHAR * {
		switch (s)
		{
			case CColRef::EUsed: return GPOS_WSZ_LIT("EUsed");
			case CColRef::EUnused: return GPOS_WSZ_LIT("EUnused");
			case CColRef::EUnknown: return GPOS_WSZ_LIT("EUnknown");
			case CColRef::ESentinel: return GPOS_WSZ_LIT("ESentinel");
		}
		return GPOS_WSZ_LIT("E<?>");
	};

	{
		CAutoTrace at(mp);
		at.Os() << "[WRV-HASHAGG-XFORM] BEGIN Transform expr_id="
				<< (ULONG_PTR) pexpr
				<< " group_cols=" << (ULONG) colref_array->Size()
				<< " minimal_group_cols="
				<< (ULONG)(popAgg->PdrgpcrMinimal()
							   ? popAgg->PdrgpcrMinimal()->Size()
							   : 0)
				<< " has_DQA_args="
				<< ((pdrgpcrArgDQA && pdrgpcrArgDQA->Size() > 0) ? 1 : 0)
				<< " agg_stage=" << (INT) popAgg->AggStage()
				<< " generates_duplicates="
				<< (popAgg->FGeneratesDuplicates() ? 1 : 0) << std::endl;

		// group cols
		for (ULONG i = 0; i < colref_array->Size(); ++i)
		{
			const CColRef *c = (*colref_array)[i];
			at.Os() << "[WRV-HASHAGG-XFORM] GROUP idx=" << i
					<< " colid=" << c->Id()
					<< " name=" << c->Name().Pstr()->GetBuffer()
					<< " usage=" << UsageToStr(c->GetUsage())
					<< " is_system=" << (c->IsSystemCol() ? 1 : 0)
					<< " is_dist=" << (c->IsDistCol() ? 1 : 0)
					<< " type_oid="
					<< CMDIdGPDB::CastMdid(c->RetrieveType()->MDId())->Oid()
					<< std::endl;
		}

		// minimal group cols（有些变换会下推后再扩）
		if (popAgg->PdrgpcrMinimal())
		{
			for (ULONG i = 0; i < popAgg->PdrgpcrMinimal()->Size(); ++i)
			{
				const CColRef *c = (*popAgg->PdrgpcrMinimal())[i];
				at.Os() << "[WRV-HASHAGG-XFORM] MINIMAL_GROUP idx=" << i
						<< " colid=" << c->Id()
						<< " name=" << c->Name().Pstr()->GetBuffer()
						<< " usage=" << UsageToStr(c->GetUsage()) << std::endl;
			}
		}

		// DQA args
		if (pdrgpcrArgDQA && pdrgpcrArgDQA->Size() > 0)
		{
			for (ULONG i = 0; i < pdrgpcrArgDQA->Size(); ++i)
			{
				const CColRef *c = (*pdrgpcrArgDQA)[i];
				at.Os() << "[WRV-HASHAGG-XFORM] DQA_ARG idx=" << i
						<< " colid=" << c->Id()
						<< " name=" << c->Name().Pstr()->GetBuffer()
						<< " usage=" << UsageToStr(c->GetUsage()) << std::endl;
			}
		}

		// 尝试打印 child 的输出列数量（判别是否“宽” / 是否可能出现 whole-row）
CDrvdPropRelational *pdpRel =
    CDrvdPropRelational::GetRelationalProperties(pexprRel->PdpDerive());
if (pdpRel)
{
    const CColRefSet *pOutput = pdpRel->GetOutputColumns();
    at.Os() << "[WRV-HASHAGG-XFORM] ChildOutputCols size="
            << (ULONG) pOutput->Size() << std::endl;

    // 可选：如果 group 列数 == 子输出列数，提示可能是“全行”分组
    if (colref_array->Size() == pOutput->Size())
    {
        at.Os() << "[WRV-HASHAGG-XFORM] NOTE group_cols == child_output_cols "
                   "(possible whole-row grouping surrogate)"
                << std::endl;
    }
}
else
{
    at.Os() << "[WRV-HASHAGG-XFORM] WARN no relational derived props for child"
            << std::endl;
}
	}
	// create alternative expression
	CExpression *pexprAlt = GPOS_NEW(mp) CExpression(
		mp,
		GPOS_NEW(mp) CPhysicalHashAgg(
			mp, colref_array, popAgg->PdrgpcrMinimal(), popAgg->Egbaggtype(),
			popAgg->FGeneratesDuplicates(), pdrgpcrArgDQA,
			CXformUtils::FMultiStageAgg(pexpr),
			CXformUtils::FAggGenBySplitDQAXform(pexpr), popAgg->AggStage(),
			!CXformUtils::FLocalAggCreatedByEagerAggXform(pexpr)),
		pexprRel, pexprScalar);
	
		{
		CAutoTrace at(mp);
		at.Os() << "[WRV-HASHAGG-XFORM] Produced PhysicalHashAgg expr_id="
				<< (ULONG_PTR) pexprAlt << " group_cols="
				<< (ULONG) colref_array->Size()
				<< " multi_stage="
				<< (CXformUtils::FMultiStageAgg(pexpr) ? 1 : 0)
				<< " split_dqa="
				<< (CXformUtils::FAggGenBySplitDQAXform(pexpr) ? 1 : 0)
				<< " local_eager_inverted="
				<< (!CXformUtils::FLocalAggCreatedByEagerAggXform(pexpr) ? 1
																		 : 0)
				<< std::endl;
	}
	// add alternative to transformation result
	pxfres->Add(pexprAlt);
		{
		CAutoTrace at(mp);
		at.Os() << "[WRV-HASHAGG-XFORM] END Transform expr_id="
				<< (ULONG_PTR) pexpr << std::endl;
	}
}

//---------------------------------------------------------------------------
//	@function:
//		CXformGbAgg2HashAgg::FApplicable
//
//	@doc:
//		Check if the transformation is applicable
//
//---------------------------------------------------------------------------
BOOL
CXformGbAgg2HashAgg::FApplicable(CExpression *pexpr)
{
	CExpression *pexprPrjList = (*pexpr)[1];
	ULONG arity = pexprPrjList->Arity();
	CMDAccessor *md_accessor = COptCtxt::PoctxtFromTLS()->Pmda();

	for (ULONG ul = 0; ul < arity; ul++)
	{
		CExpression *pexprPrjEl = (*pexprPrjList)[ul];
		CExpression *pexprAggFunc = (*pexprPrjEl)[0];
		CScalarAggFunc *popScAggFunc =
			CScalarAggFunc::PopConvert(pexprAggFunc->Pop());

		if (popScAggFunc->IsDistinct() ||
			!md_accessor->RetrieveAgg(popScAggFunc->MDId())->IsHashAggCapable())
		{
			return false;
		}
	}

	return true;
}

// EOF
