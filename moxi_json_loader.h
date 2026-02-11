#pragma once
#include <string>

namespace ic3ia {

/**
 * Convert a MoXI-JSON verification task (as used by MoXIchecker) into an
 * SMT-LIB2 script containing a VMT-style annotated list that ic3ia can
 * already load via msat_annotated_list_from_smtlib2_file().
 *
 * Semantics aligned with MoXIchecker:
 *   init_final  := init ∧ inv
 *   trans_final := trans ∧ inv'     (inv with all vars mapped to next)
 *   prop_final  := ¬query           (query is the chosen reachable formula)
 *
 * Returns true on success; on failure, returns false and (optionally) fills err.
 */
bool moxi_json_to_vmt_smt2(const std::string &json_path,
                           std::string &out_smt2,
                           std::string *err);

} // namespace ic3ia
