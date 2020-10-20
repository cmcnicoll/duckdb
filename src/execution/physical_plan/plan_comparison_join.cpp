#include "duckdb/execution/operator/join/physical_cross_product.hpp"
#include "duckdb/execution/operator/join/physical_hash_join.hpp"
#include "duckdb/execution/operator/join/physical_index_join.hpp"
#include "duckdb/execution/operator/join/physical_nested_loop_join.hpp"
#include "duckdb/execution/operator/join/physical_piecewise_merge_join.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/transaction/transaction.hpp"

using namespace std;

namespace duckdb {

static bool can_plan_index_join(Transaction &transaction, TableScanBindData *bind_data, PhysicalTableScan &scan) {
	if (!bind_data) {
		// not a table scan
		return false;
	}
	auto table = bind_data->table;
	if (transaction.storage.Find(table->storage.get())) {
		// transaction local appends: skip index join
		return false;
	}
	if (scan.table_filters && scan.table_filters->filters.size() > 0) {
		// table scan filters
		return false;
	}
	return true;
}

void TransformIndexJoin(ClientContext &context, LogicalComparisonJoin &op, Index **left_index, Index **right_index,
                        PhysicalOperator *left, PhysicalOperator *right) {
	auto &transaction = Transaction::GetTransaction(context);
	// check if one of the tables has an index on column
	if (op.join_type == JoinType::INNER && op.conditions.size() == 1) {
		// check if one of the children are table scans and if they have an index in the join attribute
		// (op.condition)
		if (left->type == PhysicalOperatorType::TABLE_SCAN) {
			auto &tbl_scan = (PhysicalTableScan &)*left;
			auto tbl = dynamic_cast<TableScanBindData *>(tbl_scan.bind_data.get());
			if (can_plan_index_join(transaction, tbl, tbl_scan)) {
				for (auto &index : tbl->table->storage->info->indexes) {
					if (index->unbound_expressions[0]->alias == op.conditions[0].left->alias) {
						*left_index = index.get();
						break;
					}
				}
			}
		}
		if (right->type == PhysicalOperatorType::TABLE_SCAN) {
			auto &tbl_scan = (PhysicalTableScan &)*right;
			auto tbl = dynamic_cast<TableScanBindData *>(tbl_scan.bind_data.get());
			if (can_plan_index_join(transaction, tbl, tbl_scan)) {
				for (auto &index : tbl->table->storage->info->indexes) {
					if (index->unbound_expressions[0]->alias == op.conditions[0].right->alias) {
						*right_index = index.get();
						break;
					}
				}
			}
		}
	}
}

unique_ptr<PhysicalOperator> PhysicalPlanGenerator::CreatePlan(LogicalComparisonJoin &op) {
	// now visit the children
	assert(op.children.size() == 2);
	idx_t lhs_cardinality = op.children[0]->EstimateCardinality();
	idx_t rhs_cardinality = op.children[1]->EstimateCardinality();
	auto left = CreatePlan(*op.children[0]);
	auto right = CreatePlan(*op.children[1]);
	assert(left && right);

	if (op.conditions.size() == 0) {
		// no conditions: insert a cross product
		return make_unique<PhysicalCrossProduct>(op.types, move(left), move(right));
	}

	bool has_equality = false;
	bool has_inequality = false;
#ifndef NDEBUG
	bool has_null_equal_conditions = false;
#endif
	for (auto &cond : op.conditions) {
		if (cond.comparison == ExpressionType::COMPARE_EQUAL) {
			has_equality = true;
		}
		if (cond.comparison == ExpressionType::COMPARE_NOTEQUAL) {
			has_inequality = true;
		}
		if (cond.null_values_are_equal) {
#ifndef NDEBUG
			has_null_equal_conditions = true;
#endif
			assert(cond.comparison == ExpressionType::COMPARE_EQUAL);
		}
	}
	unique_ptr<PhysicalOperator> plan;
	if (has_equality) {
		Index *left_index{}, *right_index{};
		TransformIndexJoin(context, op, &left_index, &right_index, left.get(), right.get());
		if (left_index && (context.force_index_join || rhs_cardinality < 0.01 * lhs_cardinality)) {
			auto &tbl_scan = (PhysicalTableScan &)*left;
			swap(op.conditions[0].left, op.conditions[0].right);
			return make_unique<PhysicalIndexJoin>(op, move(right), move(left), move(op.conditions), op.join_type,
			                                      op.right_projection_map, op.left_projection_map, tbl_scan.column_ids,
			                                      left_index, false);
		}
		if (right_index && (context.force_index_join || lhs_cardinality < 0.01 * rhs_cardinality)) {
			auto &tbl_scan = (PhysicalTableScan &)*right;
			return make_unique<PhysicalIndexJoin>(op, move(left), move(right), move(op.conditions), op.join_type,
			                                      op.left_projection_map, op.right_projection_map, tbl_scan.column_ids,
			                                      right_index, true);
		}
		// equality join: use hash join
		plan = make_unique<PhysicalHashJoin>(op, move(left), move(right), move(op.conditions), op.join_type,
		                                     op.left_projection_map, op.right_projection_map);
	} else {
		assert(!has_null_equal_conditions); // don't support this for anything but hash joins for now
		if (op.conditions.size() == 1 && !has_inequality) {
			// range join: use piecewise merge join
			plan =
			    make_unique<PhysicalPiecewiseMergeJoin>(op, move(left), move(right), move(op.conditions), op.join_type);
		} else {
			// inequality join: use nested loop
			plan = make_unique<PhysicalNestedLoopJoin>(op, move(left), move(right), move(op.conditions), op.join_type);
		}
	}
	return plan;
}

} // namespace duckdb
