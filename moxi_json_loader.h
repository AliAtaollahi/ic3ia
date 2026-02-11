#pragma once
#include <string>

namespace ic3ia {

// Convert a MoXI-JSON task into an SMT-LIB2 script with VMT-style annotations
// that ic3ia already loads via msat_annotated_list_from_smtlib2_file().
bool moxi_json_to_vmt_smt2(const std::string &json_path,
                           std::string &out_smt2,
                           std::string *err);

} // namespace ic3ia
