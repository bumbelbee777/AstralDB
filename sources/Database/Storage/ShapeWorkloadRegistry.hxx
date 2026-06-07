#pragma once



#include <Database/Storage/BulkQueryMetadata.hxx>

#include <Database/Storage/ColumnarStorage.hxx>



namespace AstralDB {



[[nodiscard]] bool ShapePrecomputeEnabled() noexcept;



void RecordObservedQueryShape(ColumnarTable &Col, const BulkQueryShape &Shape,

                              const QueryShapeFingerprint128 &Fingerprint) noexcept;



void SchedulePrecomputeForObservedLimits(ColumnarTable &Col) noexcept;



} // namespace AstralDB


