#include <libasr/containers.h>
#include <libasr/exception.h>
#include <libasr/asr_utils.h>
#include <libasr/asr_verify.h>
#include <libasr/utils.h>
#include <libasr/pass/intrinsic_function_registry.h>
#include <libasr/pass/intrinsic_array_function_registry.h>

#include <set>

namespace LCompilers {

namespace ASR {

using ASRUtils::symbol_name;
using ASRUtils::symbol_parent_symtab;

bool valid_char(char c) {
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    if (c >= '0' && c <= '9') return true;
    if (c == '_') return true;
    return false;
}

bool valid_name(const char *s) {
    if (s == nullptr) return false;
    std::string name = s;
    if (name.size() == 0) return false;
    for (size_t i=0; i<name.size(); i++) {
        if (!valid_char(s[i])) return false;
    }
    return true;
}

class VerifyVisitor : public BaseWalkVisitor<VerifyVisitor>
{
private:
    // For checking correct parent symbtab relationship
    SymbolTable *current_symtab;
    bool check_external;
    diag::Diagnostics &diagnostics;
    std::string current_name;

    // For checking that all symtabs have a unique ID.
    // We first walk all symtabs, and then we check that everything else
    // points to them (i.e., that nothing points to some symbol table that
    // is not part of this ASR).
    std::map<uint64_t,SymbolTable*> id_symtab_map;
    std::vector<std::string> function_dependencies;
    std::vector<std::string> module_dependencies;
    std::vector<std::string> variable_dependencies;

    std::set<std::pair<uint64_t, std::string>> const_assigned;

    // checks whether we've visited any `Var`, which isn't a global `Variable`
    bool non_global_symbol_visited;
    bool _return_var_or_intent_out = false;
    bool _processing_dims = false;
    const ASR::expr_t* current_expr {}; // current expression being visited 

public:
    VerifyVisitor(bool check_external, diag::Diagnostics &diagnostics) : check_external{check_external},
        diagnostics{diagnostics}, non_global_symbol_visited{false} {}

    // Requires the condition `cond` to be true. Raise an exception otherwise.
    #define require(cond, error_msg) ASRUtils::require_impl((cond), (error_msg), x.base.base.loc, diagnostics);
    #define require_with_loc(cond, error_msg, loc) ASRUtils::require_impl((cond), (error_msg), loc, diagnostics);
    // Returns true if the `symtab_ID` (sym->symtab->parent) is the current
    // symbol table `symtab` or any of its parents *and* if the symbol in the
    // symbol table is equal to `sym`. It returns false otherwise, such as in the
    // case when the symtab is in a different module or if the `sym`'s symbol table
    // does not actually contain it.
    bool symtab_in_scope(const SymbolTable *symtab, const ASR::symbol_t *sym) {
        unsigned int symtab_ID = symbol_parent_symtab(sym)->counter;
        char *sym_name = symbol_name(sym);
        const SymbolTable *s = symtab;
        while (s != nullptr) {
            if (s->counter == symtab_ID) {
                ASR::symbol_t *sym2 = s->get_symbol(sym_name);
                if (sym2) {
                    if (sym2 == sym) {
                        // The symbol table was found and the symbol `sym` is in it
                        return true;
                    } else {
                        diagnostics.message_label("The symbol table was found and the symbol in it shares the name, but is not equal to `sym`",
                        {sym->base.loc}, "failed here", diag::Level::Error, diag::Stage::ASRVerify);
                        return false;
                    }
                } else {
                    diagnostics.message_label("The symbol table was found, but the symbol `sym` is not in it",
                        {sym->base.loc}, "failed here", diag::Level::Error, diag::Stage::ASRVerify);
                    return false;
                }
            }
            s = s->parent;
        }
        diagnostics.message_label("The symbol table was not found in the scope of `symtab`.",
                        {sym->base.loc}, "failed here", diag::Level::Error, diag::Stage::ASRVerify);
        return false;
    }

    void visit_TranslationUnit(const TranslationUnit_t &x) {
        current_symtab = x.m_symtab;
        require(x.m_symtab != nullptr,
            "The TranslationUnit::m_symtab cannot be nullptr");
        require(x.m_symtab->parent == nullptr,
            "The TranslationUnit::m_symtab->parent must be nullptr");
        require(id_symtab_map.find(x.m_symtab->counter) == id_symtab_map.end(),
            "TranslationUnit::m_symtab->counter must be unique");
        require(x.m_symtab->asr_owner == (ASR::asr_t*)&x,
            "The TranslationUnit::m_symtab::asr_owner must point to itself");
        require(down_cast2<TranslationUnit_t>(current_symtab->asr_owner)->m_symtab == current_symtab,
            "The asr_owner invariant failed");
        id_symtab_map[x.m_symtab->counter] = x.m_symtab;
        for (auto &a : x.m_symtab->get_scope()) {
            this->visit_symbol(*a.second);
        }
        for (size_t i=0; i<x.n_items; i++) {
            asr_t *item = x.m_items[i];
            require(is_a<stmt_t>(*item) || is_a<expr_t>(*item),
                "TranslationUnit::m_items must be either stmt or expr");
            if (is_a<stmt_t>(*item)) {
                this->visit_stmt(*down_cast<stmt_t>(item));
            } else {
                this->visit_expr(*down_cast<expr_t>(item));
            }
        }
        current_symtab = nullptr;
    }

    void visit_Select(const Select_t& x) {
        bool fall_through = false;
        for( size_t i = 0; i < x.n_body; i++ ) {
            if( ASR::is_a<ASR::CaseStmt_t>(*x.m_body[i]) ) {
                ASR::CaseStmt_t* case_stmt_t = ASR::down_cast<ASR::CaseStmt_t>(x.m_body[i]);
                fall_through = fall_through || case_stmt_t->m_fall_through;
            }
        }
        require(fall_through == x.m_enable_fall_through,
            "Select_t::m_enable_fall_through should be " +
            std::to_string(x.m_enable_fall_through));
        BaseWalkVisitor<VerifyVisitor>::visit_Select(x);
    }

    // --------------------------------------------------------
    // symbol instances:

    void visit_Program(const Program_t &x) {
        SymbolTable *parent_symtab = current_symtab;
        current_symtab = x.m_symtab;
        require(x.m_symtab != nullptr,
            "The Program::m_symtab cannot be nullptr");
        require(x.m_symtab->parent == parent_symtab,
            "The Program::m_symtab->parent is not the right parent");
        require(x.m_symtab->parent->parent == nullptr,
            "The Program::m_symtab's parent must be TranslationUnit");
        require(id_symtab_map.find(x.m_symtab->counter) == id_symtab_map.end(),
            "Program::m_symtab->counter must be unique");
        require(x.m_symtab->asr_owner == (ASR::asr_t*)&x,
            "The X::m_symtab::asr_owner must point to X");
        require(ASRUtils::symbol_symtab(down_cast<symbol_t>(current_symtab->asr_owner)) == current_symtab,
            "The asr_owner invariant failed");
        require(x.m_name, "Program name is required");
        if (x.n_dependencies > 0) {
            require(x.m_dependencies,
            std::string(x.m_name) + "::m_dependencies is required");
        }
        id_symtab_map[x.m_symtab->counter] = x.m_symtab;
        for (auto &a : x.m_symtab->get_scope()) {
            this->visit_symbol(*a.second);
        }
        for (size_t i=0; i<x.n_body; i++) {
            LCOMPILERS_ASSERT(x.m_body[i]);
            visit_stmt(*x.m_body[i]);
        }
        current_symtab = parent_symtab;
    }

    void visit_AssociateBlock(const AssociateBlock_t& x) {
        SymbolTable *parent_symtab = current_symtab;
        current_symtab = x.m_symtab;
        require(x.m_symtab != nullptr,
            "The AssociateBlock::m_symtab cannot be nullptr");
        require(x.m_symtab->parent == parent_symtab,
            "The AssociateBlock::m_symtab->parent is not the right parent");
        require(id_symtab_map.find(x.m_symtab->counter) == id_symtab_map.end(),
            "AssociateBlock::m_symtab->counter must be unique");
        require(x.m_symtab->asr_owner == (ASR::asr_t*)&x,
            "The X::m_symtab::asr_owner must point to X");
        require(ASRUtils::symbol_symtab(down_cast<symbol_t>(current_symtab->asr_owner)) == current_symtab,
            "The asr_owner invariant failed");
        id_symtab_map[x.m_symtab->counter] = x.m_symtab;
        for (auto &a : x.m_symtab->get_scope()) {
            this->visit_symbol(*a.second);
        }
        for (size_t i=0; i<x.n_body; i++) {
            visit_stmt(*x.m_body[i]);
        }
        current_symtab = parent_symtab;
    }

    void visit_GenericProcedure(const GenericProcedure_t& x) {
        require(x.m_name != nullptr,
            "GenericProcedure::m_name cannot be nullptr");
        std::string gen_name = x.m_name;
        require(x.m_parent_symtab != nullptr,
            gen_name + "::m_parent_symtab cannot be nullptr");
        for (size_t i=0; i < x.n_procs; i++) {
            // They are already visited so just check the nullptr
            LCOMPILERS_ASSERT(x.m_procs[i]);
        }
    }

    void visit_CustomOperator(const CustomOperator_t& x) {
        require(x.m_name != nullptr,
            "CustomOperator::m_name cannot be nullptr");
        std::string cus_name = x.m_name;
        require(x.m_parent_symtab != nullptr,
            cus_name + "::m_parent_symtab cannot be nullptr");
        for (size_t i=0; i < x.n_procs; i++) {
            // They are already visited so just check the nullptr
            LCOMPILERS_ASSERT(x.m_procs[i]);
        }
    }

    void visit_Block(const Block_t& x) {
        SymbolTable *parent_symtab = current_symtab;
        current_symtab = x.m_symtab;
        require(x.m_symtab != nullptr,
            "The AssociateBlock::m_symtab cannot be nullptr");
        require(x.m_symtab->parent == parent_symtab,
            "The AssociateBlock::m_symtab->parent is not the right parent");
        require(id_symtab_map.find(x.m_symtab->counter) == id_symtab_map.end(),
            "AssociateBlock::m_symtab->counter must be unique");
        require(x.m_symtab->asr_owner == (ASR::asr_t*)&x,
            "The X::m_symtab::asr_owner must point to X");
        require(ASRUtils::symbol_symtab(down_cast<symbol_t>(current_symtab->asr_owner)) == current_symtab,
            "The asr_owner invariant failed");
        id_symtab_map[x.m_symtab->counter] = x.m_symtab;
        for (auto &a : x.m_symtab->get_scope()) {
            this->visit_symbol(*a.second);
        }
        for (size_t i=0; i<x.n_body; i++) {
            visit_stmt(*x.m_body[i]);
        }
        current_symtab = parent_symtab;
    }

    void visit_Requirement(const Requirement_t& x) {
        SymbolTable *parent_symtab = current_symtab;
        current_symtab = x.m_symtab;
        require(x.m_symtab != nullptr,
            "The Requirement::m_symtab cannot be nullptr");
        require(x.m_symtab->parent == parent_symtab,
            "The Requirement::m_symtab->parent is not the right parent");
        require(id_symtab_map.find(x.m_symtab->counter) == id_symtab_map.end(),
            "Requirement::m_symtab->counter must be unique");
        require(x.m_symtab->asr_owner == (ASR::asr_t*)&x,
            "The X::m_symtab::asr_owner must point to X");
        require(ASRUtils::symbol_symtab(down_cast<symbol_t>(current_symtab->asr_owner)) == current_symtab,
            "The asr_owner invariant failed");
        id_symtab_map[x.m_symtab->counter] = x.m_symtab;
        for (auto &a : x.m_symtab->get_scope()) {
            this->visit_symbol(*a.second);
        }
        current_symtab = parent_symtab;
    }

    void visit_Template(const Template_t& x) {
        SymbolTable *parent_symtab = current_symtab;
        current_symtab = x.m_symtab;
        require(x.m_symtab != nullptr,
            "The Requirement::m_symtab cannot be nullptr");
        require(x.m_symtab->parent == parent_symtab,
            "The Requirement::m_symtab->parent is not the right parent");
        require(id_symtab_map.find(x.m_symtab->counter) == id_symtab_map.end(),
            "Requirement::m_symtab->counter must be unique");
        require(x.m_symtab->asr_owner == (ASR::asr_t*)&x,
            "The X::m_symtab::asr_owner must point to X");
        require(ASRUtils::symbol_symtab(down_cast<symbol_t>(current_symtab->asr_owner)) == current_symtab,
            "The asr_owner invariant failed");
        id_symtab_map[x.m_symtab->counter] = x.m_symtab;
        for (auto &a : x.m_symtab->get_scope()) {
            this->visit_symbol(*a.second);
        }
        current_symtab = parent_symtab;
    }

    void visit_BlockCall(const BlockCall_t& x) {
        require(x.m_m != nullptr, "Block call made to inexisting block");
        require(symtab_in_scope(current_symtab, x.m_m),
            "Block " + std::string(ASRUtils::symbol_name(x.m_m)) +
            " should resolve in current scope.");
        SymbolTable *parent_symtab = current_symtab;
        ASR::Block_t* block = ASR::down_cast<ASR::Block_t>(x.m_m);
        LCOMPILERS_ASSERT(block); // already checked above, just making sure
        current_symtab = block->m_symtab;
        for (size_t i=0; i<block->n_body; i++) {
            visit_stmt(*(block->m_body[i]));
        }
        current_symtab = parent_symtab;
    }

    void verify_unique_dependencies(char** m_dependencies,
        size_t n_dependencies, std::string m_name, const Location& loc) {
        // Check if any dependency is duplicated
        // in the dependency list of the function
        std::set<std::string> dependencies_set;
        for( size_t i = 0; i < n_dependencies; i++ ) {
            std::string found_dep = m_dependencies[i];
            require_with_loc(dependencies_set.find(found_dep) == dependencies_set.end(),
                    "Symbol " + found_dep + " is duplicated in the dependency "
                    "list of " + m_name, loc);
            dependencies_set.insert(found_dep);
        }
    }

    void visit_Module(const Module_t &x) {
        module_dependencies.clear();
        module_dependencies.reserve(x.n_dependencies);
        SymbolTable *parent_symtab = current_symtab;
        current_symtab = x.m_symtab;
        require(x.m_symtab != nullptr,
            "The Module::m_symtab cannot be nullptr");
        require(x.m_symtab->parent == parent_symtab,
            "The Module::m_symtab->parent is not the right parent");
        require(x.m_symtab->parent->parent == nullptr,
            "The Module::m_symtab's parent must be TranslationUnit");
        require(id_symtab_map.find(x.m_symtab->counter) == id_symtab_map.end(),
            "Module::m_symtab->counter must be unique");
        require(x.m_symtab->asr_owner == (ASR::asr_t*)&x,
            "The X::m_symtab::asr_owner must point to X");
        require(x.m_name, "Module name is required");
        require(ASRUtils::symbol_symtab(down_cast<symbol_t>(current_symtab->asr_owner)) == current_symtab,
            "The asr_owner invariant failed");
        id_symtab_map[x.m_symtab->counter] = x.m_symtab;
        for (auto &a : x.m_symtab->get_scope()) {
            this->visit_symbol(*a.second);
        }

        verify_unique_dependencies(x.m_dependencies, x.n_dependencies,
                                   x.m_name, x.base.base.loc);

        for (size_t i=0; i < x.n_dependencies; i++) {
            require(x.m_dependencies[i] != nullptr,
                "A module dependency must not be a nullptr");
            require(std::string(x.m_dependencies[i]) != "",
                "A module dependency must not be an empty string");
            require(valid_name(x.m_dependencies[i]),
                "A module dependency must be a valid string");
        }
        for( auto& dep: module_dependencies ) {
            if( dep != x.m_name ) {
                require(present(x.m_dependencies, x.n_dependencies, dep),
                        "Module " + std::string(x.m_name) +
                        " dependencies must contain " + dep +
                        " because a function present in it is getting called in "
                        + std::string(x.m_name) + ".");
            }
        }
        current_symtab = parent_symtab;
    }

    void visit_Assignment(const Assignment_t& x) {
        ASR::expr_t* target = x.m_target;
        if( ASR::is_a<ASR::Var_t>(*target) ) {
            ASR::Var_t* target_Var = ASR::down_cast<ASR::Var_t>(target);
            bool is_target_const = false;
            ASR::ttype_t* target_type = nullptr;
            ASR::symbol_t* target_sym = ASRUtils::symbol_get_past_external(target_Var->m_v);
            if( target_sym && ASR::is_a<ASR::Variable_t>(*target_sym) ) {
                ASR::Variable_t* var = ASR::down_cast<ASR::Variable_t>(target_sym);
                require(var->m_intent != ASR::intentType::In, "Assignment target `"
                    + std::string(var->m_name) + "` with intent `IN` not allowed");
                target_type = var->m_type;
                is_target_const = var->m_storage == ASR::storage_typeType::Parameter;
            }
            if( is_target_const ) {
                std::string variable_name = ASRUtils::symbol_name(target_Var->m_v);
                require(const_assigned.find(std::make_pair(current_symtab->counter,
                    variable_name)) == const_assigned.end(),
                    "Assignment target with " + ASRUtils::type_to_str_python(target_type)
                    + " cannot be re-assigned.");
                const_assigned.insert(std::make_pair(current_symtab->counter, variable_name));
            }
        }
        // it's possible that the target is an external symbol, and during
        // initial deserialization pass, so we don't do the below verification
        if ( check_external && x.m_realloc_lhs ) {
            ASR::expr_t* a_target = x.m_target;
            bool is_allocatable = ASRUtils::is_allocatable(a_target);
            if ( ASR::is_a<ASR::StructInstanceMember_t>(*a_target) ) {
                ASR::StructInstanceMember_t* a_target_struct = ASR::down_cast<ASR::StructInstanceMember_t>(a_target);
                is_allocatable |= ASRUtils::is_allocatable(a_target_struct->m_v);
            }
            require(is_allocatable,
                "Reallocation of non allocatable variable is not allowed");
        }
        BaseWalkVisitor<VerifyVisitor>::visit_Assignment(x);
    }

    void visit_StructMethodDeclaration(const StructMethodDeclaration_t &x) {
        require(x.m_name != nullptr,
            "The StructMethodDeclaration::m_name cannot be nullptr");
        require(x.m_proc != nullptr,
            "The StructMethodDeclaration::m_proc cannot be nullptr");
        require(x.m_proc_name != nullptr,
            "The StructMethodDeclaration::m_proc_name cannot be nullptr");

        SymbolTable *symtab = x.m_parent_symtab;
        require(symtab != nullptr,
            "StructMethodDeclaration::m_parent_symtab cannot be nullptr");
        require(symtab->get_symbol(std::string(x.m_name)) != nullptr,
            "StructMethodDeclaration '" + std::string(x.m_name) + "' not found in parent_symtab symbol table");
        symbol_t *symtab_sym = symtab->get_symbol(std::string(x.m_name));
        const symbol_t *current_sym = &x.base;
        require(symtab_sym == current_sym,
            "StructMethodDeclaration's parent symbol table does not point to it");
        require(id_symtab_map.find(symtab->counter) != id_symtab_map.end(),
            "StructMethodDeclaration::m_parent_symtab must be present in the ASR ("
                + std::string(x.m_name) + ")");

        ASR::Function_t* x_m_proc = ASR::down_cast<ASR::Function_t>(x.m_proc);
        if( x.m_self_argument ) {
            bool arg_found = false;
            std::string self_arg_name = std::string(x.m_self_argument);
            for( size_t i = 0; i < x_m_proc->n_args; i++ ) {
                std::string arg_name = std::string(ASRUtils::symbol_name(
                    ASR::down_cast<ASR::Var_t>(x_m_proc->m_args[i])->m_v));
                if( self_arg_name == arg_name ) {
                    arg_found = true;
                    break ;
                }
            }
            require(arg_found, self_arg_name + " must be present in " +
                    std::string(x.m_name) + " procedures.");
        }
    }

    void visit_Function(const Function_t &x) {
        std::vector<std::string> function_dependencies_copy = function_dependencies;
        function_dependencies.clear();
        function_dependencies.reserve(x.n_dependencies);
        SymbolTable *parent_symtab = current_symtab;
        current_symtab = x.m_symtab;
        require(x.m_symtab != nullptr,
            "The Function::m_symtab cannot be nullptr");
        require(x.m_symtab->parent == parent_symtab,
            "The Function::m_symtab->parent is not the right parent");
        require(x.m_symtab->asr_owner == (ASR::asr_t*)&x,
            "The X::m_symtab::asr_owner must point to X");
        require(id_symtab_map.find(x.m_symtab->counter) == id_symtab_map.end(),
            "Function::m_symtab->counter must be unique");
        require(ASRUtils::symbol_symtab(down_cast<symbol_t>(current_symtab->asr_owner)) == current_symtab,
            "The asr_owner invariant failed");
        require(x.m_name, "Function name is required");
        std::string func_name = x.m_name;
        require(x.m_function_signature,
                    "Type signature is required for `" + func_name + "`");
        id_symtab_map[x.m_symtab->counter] = x.m_symtab;
        for (auto &a : x.m_symtab->get_scope()) {
            LCOMPILERS_ASSERT(a.second);
            this->visit_symbol(*a.second);
        }
        visit_ttype(*x.m_function_signature);
        for (size_t i=0; i<x.n_args; i++) {
            LCOMPILERS_ASSERT(x.m_args[i]);
            visit_expr(*x.m_args[i]);
        }
        for (size_t i=0; i<x.n_body; i++) {
            LCOMPILERS_ASSERT(x.m_body[i]);
            visit_stmt(*x.m_body[i]);
        }
        if (x.m_return_var) {
            visit_expr(*x.m_return_var);
        }

        verify_unique_dependencies(x.m_dependencies, x.n_dependencies,
                                   x.m_name, x.base.base.loc);

        // Get the x parent symtab.
        SymbolTable *x_parent_symtab = x.m_symtab->parent;

        // Dependencies of the function should be from function's parent symbol table.
        for( size_t i = 0; i < x.n_dependencies; i++ ) {
            std::string found_dep = x.m_dependencies[i];

            // Get the symbol of the found_dep.
            ASR::symbol_t* dep_sym = x_parent_symtab->resolve_symbol(found_dep);

            require(dep_sym != nullptr,
                            "Dependency " + found_dep +  " is inside symbol table " + std::string(x.m_name));
        }
        // Check if there are unnecessary dependencies
        // present in the dependency list of the function
        for( size_t i = 0; i < x.n_dependencies; i++ ) {
            std::string found_dep = x.m_dependencies[i];
            require(std::find(function_dependencies.begin(), function_dependencies.end(), found_dep) != function_dependencies.end(),
                    "Function " + std::string(x.m_name) + " doesn't depend on " + found_dep +
                    " but is found in its dependency list.");
        }

        // Check if all the dependencies found are
        // present in the dependency list of the function
        for( auto& found_dep: function_dependencies ) {
            require(present(x.m_dependencies, x.n_dependencies, found_dep),
                    "Function " + std::string(x.m_name) + " depends on " + found_dep +
                    " but isn't found in its dependency list.");
        }

        require(ASRUtils::get_FunctionType(x)->n_arg_types == x.n_args,
            "Number of argument types in FunctionType must be exactly same as "
            "number of arguments in the function");

        visit_ttype(*x.m_function_signature);
        current_symtab = parent_symtab;
        function_dependencies = function_dependencies_copy;
    }

    template <typename T>
    void visit_UserDefinedType(const T &x) {
        SymbolTable *parent_symtab = current_symtab;
        current_symtab = x.m_symtab;
        require(x.m_name != nullptr,
            "The Struct::m_name cannot be nullptr");
        require(x.m_symtab != nullptr,
            "The Struct::m_symtab cannot be nullptr");
        require(x.m_symtab->parent == parent_symtab,
            "The Struct::m_symtab->parent is not the right parent");
        require(x.m_symtab->asr_owner == (ASR::asr_t*)&x,
            "The X::m_symtab::asr_owner must point to X");
        require(id_symtab_map.find(x.m_symtab->counter) == id_symtab_map.end(),
            "Struct::m_symtab->counter must be unique");
        require(ASRUtils::symbol_symtab(down_cast<symbol_t>(current_symtab->asr_owner)) == current_symtab,
            "The asr_owner invariant failed");
        id_symtab_map[x.m_symtab->counter] = x.m_symtab;
        std::vector<std::string> struct_dependencies;
        for (auto &a : x.m_symtab->get_scope()) {
            this->visit_symbol(*a.second);
            if( ASR::is_a<ASR::StructMethodDeclaration_t>(*a.second) ||
                ASR::is_a<ASR::GenericProcedure_t>(*a.second) ||
                ASR::is_a<ASR::Struct_t>(*a.second) ||
                ASR::is_a<ASR::Union_t>(*a.second) ||
                ASR::is_a<ASR::ExternalSymbol_t>(*a.second) ||
                ASR::is_a<ASR::CustomOperator_t>(*a.second) ) {
                continue ;
            }
            if ( ASR::is_a<ASR::Variable_t>(*a.second) ) {
                ASR::Variable_t* var = ASR::down_cast<ASR::Variable_t>(a.second);
                if ( var->m_type_declaration ) {
                    struct_dependencies.push_back(
                        std::string(ASRUtils::symbol_name(var->m_type_declaration)));
                }
            }
            // TODO: Uncomment the following line
            // ASR::ttype_t* var_type = ASRUtils::extract_type(ASRUtils::symbol_type(a.second));
            ASR::ttype_t* var_type = ASRUtils::type_get_past_pointer(ASRUtils::symbol_type(a.second));
            char* aggregate_type_name = nullptr;
            ASR::symbol_t* sym = nullptr;
            if( ASR::is_a<ASR::EnumType_t>(*var_type) ) {
                sym = ASR::down_cast<ASR::EnumType_t>(var_type)->m_enum_type;
                aggregate_type_name = ASRUtils::symbol_name(sym);
            } else if( ASR::is_a<ASR::UnionType_t>(*var_type) ) {
                sym = ASR::down_cast<ASR::UnionType_t>(var_type)->m_union_type;
                aggregate_type_name = ASRUtils::symbol_name(sym);
            }
            if( aggregate_type_name && ASRUtils::symbol_parent_symtab(sym) != current_symtab ) {
                struct_dependencies.push_back(std::string(aggregate_type_name));
                require(present(x.m_dependencies, x.n_dependencies, std::string(aggregate_type_name)),
                    std::string(x.m_name) + " depends on " + std::string(aggregate_type_name)
                    + " but it isn't found in its dependency list.");
            }
        }
        for( size_t i = 0; i < x.n_dependencies; i++ ) {
            require(std::find(struct_dependencies.begin(), struct_dependencies.end(),
                    std::string(x.m_dependencies[i])) != struct_dependencies.end(),
                std::string(x.m_dependencies[i]) + " is not a dependency of " + std::string(x.m_name)
                + " but it is present in its dependency list.");
        }

        verify_unique_dependencies(x.m_dependencies, x.n_dependencies,
                                   x.m_name, x.base.base.loc);
        current_symtab = parent_symtab;
    }

    void visit_Struct(const Struct_t& x) {
        visit_UserDefinedType(x);
        if( !x.m_alignment ) {
            return ;
        }
        ASR::expr_t* aligned_expr_value = ASRUtils::expr_value(x.m_alignment);
        std::string msg = "Alignment should always evaluate to a constant expressions.";
        require(aligned_expr_value, msg);
        int64_t alignment_int = 0;
        require(ASRUtils::extract_value(aligned_expr_value, alignment_int), msg);
        require(alignment_int != 0 && (alignment_int & (alignment_int - 1)) == 0,
                "Alignment " + std::to_string(alignment_int) +
                " is not a positive power of 2.");
    }

    void visit_Enum(const Enum_t& x) {
        visit_UserDefinedType(x);
        require(x.m_type != nullptr,
            "The common type of EnumType cannot be nullptr. " +
            std::string(x.m_name) + " doesn't seem to follow this rule.");
        ASR::ttype_t* common_type = x.m_type;
        std::map<int64_t, int64_t> value2count;
        for( auto itr: x.m_symtab->get_scope() ) {
            ASR::Variable_t* itr_var = ASR::down_cast<ASR::Variable_t>(itr.second);
            require(itr_var->m_symbolic_value != nullptr,
                "All members of EnumType must have their values to be set. " +
                std::string(itr_var->m_name) + " doesn't seem to follow this rule in "
                + std::string(x.m_name) + " EnumType.");
            require(ASRUtils::check_equal_type(itr_var->m_type, common_type),
                "All members of EnumType must the same type. " +
                std::string(itr_var->m_name) + " doesn't seem to follow this rule in " +
                std::string(x.m_name) + " EnumType.");
            ASR::expr_t* value = ASRUtils::expr_value(itr_var->m_symbolic_value);
            int64_t value_int64 = -1;
            ASRUtils::extract_value(value, value_int64);
            if( value2count.find(value_int64) == value2count.end() ) {
                value2count[value_int64] = 0;
            }
            value2count[value_int64] += 1;
        }

        bool is_enumtype_correct = false;
        bool is_enum_integer = ASR::is_a<ASR::Integer_t>(*x.m_type);
        if( x.m_enum_value_type == ASR::enumtypeType::IntegerConsecutiveFromZero ) {
            is_enumtype_correct = (is_enum_integer &&
                                   (value2count.find(0) != value2count.end()) &&
                                   (value2count.size() == x.n_members));
            int64_t prev = -1;
            if( is_enumtype_correct ) {
                for( auto enum_value: value2count ) {
                    if( enum_value.first - prev != 1 ) {
                        is_enumtype_correct = false;
                        break ;
                    }
                    prev = enum_value.first;
                }
            }
        } else if( x.m_enum_value_type == ASR::enumtypeType::IntegerNotUnique ) {
            is_enumtype_correct = is_enum_integer && (value2count.size() != x.n_members);
        } else if( x.m_enum_value_type == ASR::enumtypeType::IntegerUnique ) {
            is_enumtype_correct = is_enum_integer && (value2count.size() == x.n_members);
        } else if( x.m_enum_value_type == ASR::enumtypeType::NonInteger ) {
            is_enumtype_correct = !is_enum_integer;
        }
        require(is_enumtype_correct, "Properties of enum value members don't match correspond "
                                     "to Enum::m_enum_value_type");
    }

    void visit_Union(const Union_t& x) {
        visit_UserDefinedType(x);
    }

    void visit_Variable(const Variable_t &x) {
        std::string current_name_copy = current_name;
        current_name = x.m_name;
        variable_dependencies.clear();
        SymbolTable *symtab = x.m_parent_symtab;
        require(symtab != nullptr,
            "Variable::m_parent_symtab cannot be nullptr");
        require(symtab->get_symbol(std::string(x.m_name)) != nullptr,
            "Variable '" + std::string(x.m_name) + "' not found in parent_symtab symbol table");
        symbol_t *symtab_sym = symtab->get_symbol(std::string(x.m_name));
        const symbol_t *current_sym = &x.base;
        require(symtab_sym == current_sym,
            "Variable's parent symbol table does not point to it");
        require(current_symtab == symtab,
            "Variable's parent-symbolTable and actuall parent symbolTable don't match (Maybe inserted from another symbolTable)");
        require(id_symtab_map.find(symtab->counter) != id_symtab_map.end(),
            "Variable::m_parent_symtab must be present in the ASR ("
                + std::string(x.m_name) + ")");

        ASR::asr_t* asr_owner = symtab->asr_owner;
        bool is_module = false, is_struct = false;
        if( ASR::is_a<ASR::symbol_t>(*asr_owner)) {
            ASR::symbol_t* asr_owner_sym = ASR::down_cast<ASR::symbol_t>(asr_owner);
            if (ASR::is_a<ASR::Module_t>(*asr_owner_sym)) {
                is_module = true;
            }
            if (ASR::is_a<ASR::Struct_t>(*asr_owner_sym)) {
                is_struct = true;
            }
        }
        if( symtab->parent != nullptr &&
            !is_module && !is_struct) {
            // For now restrict this check only to variables which are present
            // inside symbols which have a body.
            ASR::ArrayConstructor_t *array_construct = nullptr;
            if (x.m_symbolic_value && ASR::is_a<ASR::ArrayConstructor_t>(*x.m_symbolic_value)) {
                array_construct = ASR::down_cast<ASR::ArrayConstructor_t>(x.m_symbolic_value);
            }

            if (array_construct && array_construct->n_args > 0 && ASR::is_a<ASR::StructConstructor_t>(*array_construct->m_args[0])) {
                for (size_t j = 0; j < array_construct->n_args; j++) {
                    require( (x.m_symbolic_value == nullptr && x.m_value == nullptr) ||
                            (x.m_symbolic_value != nullptr && x.m_value != nullptr) ||
                            (x.m_symbolic_value != nullptr && ASRUtils::is_value_constant(array_construct->m_args[j])),
                            "Initialisation of " + std::string(x.m_name) +
                            " must reduce to a compile time constant.");
                }
            } else {
                require( (x.m_symbolic_value == nullptr && x.m_value == nullptr) ||
                        (x.m_symbolic_value != nullptr && x.m_value != nullptr) ||
                        (x.m_symbolic_value != nullptr && ASRUtils::is_value_constant(x.m_symbolic_value)),
                        "Initialisation of " + std::string(x.m_name) +
                        " must reduce to a compile time constant.");
            }
        }
        if(ASRUtils::is_character(*x.m_type)){
            String_t* str = down_cast<String_t>(ASRUtils::extract_type(x.m_type));
            require(str->m_len_kind != ASR::ImplicitLength,
                "Variable symbol of string type can't have a length of kind \"ImplicitLength\"")
            if(str->m_len_kind == ASR::DeferredLength){
                /* 
                    String type Varaible + DeferredLength ==> Must be allocatable or pointer(atleast for Fortran frontend)
                    String type Expressions + DeferredLength ==> Dont' have to be allocatable or pointer.
                */ 
                require(ASRUtils::is_allocatable(x.m_type) || ASRUtils::is_pointer(x.m_type) ,
                    "Variable of string type with length kind \"DeferredLength\" must be allocatable OR pointer");
            }
            if(x.m_abi == abiType::BindC && 
                x.m_intent != ASR::Local /*Input OR Output*/){
                if(ASRUtils::is_string_only(x.m_type) && 
                    str->m_physical_type == CChar){ // Exclude array of strings
                    require(str->m_len_kind == ASR::ExpressionLength, 
                        "Cbind character variable that isn't local must have length kind \"ExpressionLength\"");
                    int64_t len = 0; ASRUtils::extract_value(str->m_len, len);
                    require(len == 1,
                        "Cbind character variable that isn't local must have length 1");
                }
            }
            if(str->m_physical_type == ASR::CChar){
                require(x.m_intent != ASR::Local,
                    "CChar-string-physical type shouldn't be used with local variables");
            }
        }
        if (x.m_symbolic_value)
            visit_expr(*x.m_symbolic_value);
        if (x.m_value)
             visit_expr(*x.m_value);
        _return_var_or_intent_out = x.m_intent == ASR::intentType::Out ||
                                    x.m_intent == ASR::intentType::InOut ||
                                    x.m_intent == ASR::intentType::ReturnVar;
        visit_ttype(*x.m_type);
        _return_var_or_intent_out = false;

        verify_unique_dependencies(x.m_dependencies, x.n_dependencies,
                                   x.m_name, x.base.base.loc);

        // Verify dependencies
        for( size_t i = 0; i < x.n_dependencies; i++ ) {
            require(std::find(
                variable_dependencies.begin(),
                variable_dependencies.end(),
                std::string(x.m_dependencies[i])
            ) != variable_dependencies.end(),
                "Variable " + std::string(x.m_name) + " doesn't depend on " +
                std::string(x.m_dependencies[i]) + " but is found in its dependency list.");
        }

        for( size_t i = 0; i < variable_dependencies.size(); i++ ) {
            require(present(x.m_dependencies, x.n_dependencies, variable_dependencies[i]),
                "Variable " + std::string(x.m_name) + " depends on " +
                std::string(variable_dependencies[i]) + " but isn't found in its dependency list.");
        }
        if ( ASR::is_a<ASR::StructType_t>(*ASRUtils::extract_type(x.m_type)) ) {
            require(x.m_type_declaration != nullptr,
                "Variable " + std::string(x.m_name) + " of type StructType must have a type declaration.");
        }
        current_name = current_name_copy;
    }

    void visit_expr(const expr_t &b){
        const ASR::expr_t* expr_tmp = current_expr;
        current_expr = &b;
        BaseWalkVisitor<VerifyVisitor>::visit_expr(b);
        current_expr = expr_tmp;
    }
    
    void visit_ExternalSymbol(const ExternalSymbol_t &x) {
        if (check_external) {
            require(x.m_external != nullptr,
                "ExternalSymbol::m_external cannot be nullptr");
            require(!is_a<ExternalSymbol_t>(*x.m_external),
                "ExternalSymbol::m_external cannot be an ExternalSymbol");
            char *orig_name = symbol_name(x.m_external);
            require(std::string(x.m_original_name) == std::string(orig_name),
                "ExternalSymbol::m_original_name must match external->m_name");
            ASR::Module_t *m = ASRUtils::get_sym_module(x.m_external);
            ASR::Struct_t* sm = nullptr;
            ASR::Enum_t* em = nullptr;
            ASR::Union_t* um = nullptr;
            ASR::Function_t* fm = nullptr;
            bool is_valid_owner = false;
            is_valid_owner = m != nullptr && ((ASR::symbol_t*) m == ASRUtils::get_asr_owner(x.m_external));
            std::string asr_owner_name = "";
            if( !is_valid_owner ) {
                ASR::symbol_t* asr_owner_sym = ASRUtils::get_asr_owner(x.m_external);
                is_valid_owner = (ASR::is_a<ASR::Struct_t>(*asr_owner_sym) ||
                                  ASR::is_a<ASR::Enum_t>(*asr_owner_sym) ||
                                  ASR::is_a<ASR::Function_t>(*asr_owner_sym) ||
                                  ASR::is_a<ASR::Union_t>(*asr_owner_sym));
                if( ASR::is_a<ASR::Struct_t>(*asr_owner_sym) ) {
                    sm = ASR::down_cast<ASR::Struct_t>(asr_owner_sym);
                    asr_owner_name = sm->m_name;
                } else if( ASR::is_a<ASR::Enum_t>(*asr_owner_sym) ) {
                    em = ASR::down_cast<ASR::Enum_t>(asr_owner_sym);
                    asr_owner_name = em->m_name;
                } else if( ASR::is_a<ASR::Union_t>(*asr_owner_sym) ) {
                    um = ASR::down_cast<ASR::Union_t>(asr_owner_sym);
                    asr_owner_name = um->m_name;
                } else if( ASR::is_a<ASR::Function_t>(*asr_owner_sym) ) {
                    fm = ASR::down_cast<ASR::Function_t>(asr_owner_sym);
                    asr_owner_name = fm->m_name;
                }
            } else {
                asr_owner_name = m->m_name;
            }
            std::string x_m_module_name = x.m_module_name;
            if( current_symtab->resolve_symbol(x.m_module_name) ) {
                x_m_module_name = ASRUtils::symbol_name(
                    ASRUtils::symbol_get_past_external(
                        current_symtab->resolve_symbol(x.m_module_name)));
            }
            require(is_valid_owner,
                "ExternalSymbol::m_external '" + std::string(x.m_name) + "' is not in a module or struct type, owner: " +
                x_m_module_name);
            require(x_m_module_name == asr_owner_name,
                "ExternalSymbol::m_module_name `" + x_m_module_name
                + "` must match external's module name `" + asr_owner_name + "`");
            ASR::symbol_t *s = nullptr;
            if( m != nullptr && ((ASR::symbol_t*) m == ASRUtils::get_asr_owner(x.m_external)) ) {
                s = m->m_symtab->find_scoped_symbol(x.m_original_name, x.n_scope_names, x.m_scope_names);
            } else if( sm ) {
                s = sm->m_symtab->resolve_symbol(std::string(x.m_original_name));
            } else if( em ) {
                s = em->m_symtab->resolve_symbol(std::string(x.m_original_name));
            } else if( fm ) {
                s = fm->m_symtab->resolve_symbol(std::string(x.m_original_name));
            } else if( um ) {
                s = um->m_symtab->resolve_symbol(std::string(x.m_original_name));
            }
            require(s != nullptr,
                "ExternalSymbol::m_original_name ('"
                + std::string(x.m_original_name)
                + "') + scope_names not found in a module '"
                + asr_owner_name + "'");
            require(s == x.m_external,
                std::string("ExternalSymbol::m_name + scope_names found but not equal to m_external, ") +
                "original_name " + std::string(x.m_original_name) + ".");
        }
    }

    // --------------------------------------------------------
    // nodes that have symbol in their fields:

    void visit_Var(const Var_t &x) {
        require(x.m_v != nullptr,
            "Var_t::m_v cannot be nullptr");
        std::string x_mv_name = ASRUtils::symbol_name(x.m_v);
        ASR::symbol_t *s = x.m_v;
        if (check_external) {
            s = ASRUtils::symbol_get_past_external(x.m_v);
        }
        if (is_a<ASR::Variable_t>(*s) && is_a<ASR::ExternalSymbol_t>(*x.m_v)) {
            non_global_symbol_visited = false;
        } else {
            non_global_symbol_visited = true;
        }
        require(is_a<Variable_t>(*s) || is_a<Function_t>(*s)
                || is_a<ASR::Enum_t>(*s) || is_a<ASR::ExternalSymbol_t>(*s),
            "Var_t::m_v " + x_mv_name + " does not point to a Variable_t, " \
            "Function_t, or Enum_t (possibly behind ExternalSymbol_t)");
        require(symtab_in_scope(current_symtab, x.m_v),
            "Var::m_v `" + x_mv_name + "` cannot point outside of its symbol table");
        if ( x_mv_name != current_name ) {
            variable_dependencies.push_back(x_mv_name);
        }
    }

    void visit_ImplicitDeallocate(const ImplicitDeallocate_t &x) {
        // TODO: check that every allocated variable is deallocated.
        BaseWalkVisitor::visit_ImplicitDeallocate(x);
    }

    void check_var_external(const ASR::expr_t &x) {
        if (ASR::is_a<ASR::Var_t>(x)) {
            ASR::symbol_t *s = ((ASR::Var_t*)&x)->m_v;
            if (ASR::is_a<ASR::ExternalSymbol_t>(*s)) {
                ASR::ExternalSymbol_t *e = ASR::down_cast<ASR::ExternalSymbol_t>(s);
                ASRUtils::require_impl(e->m_external, "m_external cannot be null here",
                        x.base.loc, diagnostics);
            }
        }
    }

    template <typename T>
    void handle_ArrayItemSection(const T &x) {
        visit_expr(*x.m_v);
        for (size_t i=0; i<x.n_args; i++) {
            if( x.m_args[i].m_step != nullptr ) {
                require_with_loc(x.m_args[i].m_left != nullptr &&
                                 x.m_args[i].m_right != nullptr,
                    "Sliced dimension should always have lower and "
                    "upper bounds present.", x.base.base.loc);
            }
            visit_array_index(x.m_args[i]);
        }
        require(x.m_type != nullptr,
            "ArrayItemSection::m_type cannot be nullptr");
        visit_ttype(*x.m_type);
        if (check_external) {
            check_var_external(*x.m_v);
            int n_dims = ASRUtils::extract_n_dims_from_ttype(
                    ASRUtils::expr_type(x.m_v));
            if (ASR::is_a<ASR::String_t>(*x.m_type) && n_dims == 0) {
                // TODO: This seems like a bug, we should not use ArrayItem with
                // strings but StringItem. For now we ignore it, but we should
                // fix it
            } else {
                require(n_dims > 0,
                    "The variable in ArrayItem must be an array, not a scalar");
            }
        }
    }

    void visit_ArrayItem(const ArrayItem_t &x) {
        if( check_external ) {
            if( ASRUtils::is_array_indexed_with_array_indices(x.m_args, x.n_args) ) {
                require(ASRUtils::is_array(x.m_type),
                    "ArrayItem::m_type with array indices must be an array.")
            } else {
                require(!ASRUtils::is_array(x.m_type),
                    "ArrayItem::m_type cannot be array.")
            }
        }
        handle_ArrayItemSection(x);
    }

    void visit_ArraySize(const ArraySize_t& x) {
        if (check_external) {
            require(ASRUtils::is_array(ASRUtils::expr_type(x.m_v)),
                "ArraySize::m_v must be an array");
        }
        BaseWalkVisitor<VerifyVisitor>::visit_ArraySize(x);
    }

    void visit_ArraySection(const ArraySection_t &x) {
        require(
            ASR::is_a<ASR::Array_t>(*x.m_type),
            "ArrayItemSection::m_type can only be an Array"
        );
        handle_ArrayItemSection(x);
    }

    template <typename T>
    void verify_args(const T& x) {
        ASR::symbol_t* func_sym = ASRUtils::symbol_get_past_external(x.m_name);
        ASR::Function_t* func = nullptr;
        if( func_sym && ASR::is_a<ASR::Function_t>(*func_sym) ) {
            func = ASR::down_cast<ASR::Function_t>(func_sym);
        }

        if( func ) {
            for (size_t i = 0; i < x.n_args; i++) {
                ASR::symbol_t* arg_sym = ASR::down_cast<ASR::Var_t>(func->m_args[i])->m_v;
                if (x.m_args[i].m_value == nullptr &&
                    (ASR::is_a<ASR::Variable_t>(*arg_sym) &&
                        ASR::down_cast<ASR::Variable_t>(arg_sym)->m_presence !=
                        ASR::presenceType::Optional)) {

                        require(false, "Required argument " +
                                    std::string(ASRUtils::symbol_name(arg_sym)) +
                                    " cannot be nullptr.");

                }
            }
        }

        for (size_t i=0; i<x.n_args; i++) {
            if( x.m_args[i].m_value ) {
                visit_expr(*(x.m_args[i].m_value));
            }
        }
    }

    void visit_ArrayPhysicalCast(const ASR::ArrayPhysicalCast_t& x) {
        BaseWalkVisitor<VerifyVisitor>::visit_ArrayPhysicalCast(x);
        if( x.m_old != ASR::array_physical_typeType::DescriptorArray ) {
            require(x.m_new != x.m_old, "ArrayPhysicalCast is redundant, "
                "the old physical type and new physical type must be different.");
        }
        if(check_external){
            require(x.m_new == ASRUtils::extract_physical_type(x.m_type),
                "Destination physical type conflicts with the physical type of target");
            require(x.m_old == ASRUtils::extract_physical_type(ASRUtils::expr_type(x.m_arg)),
                "Old physical type conflicts with the physical type of argument " + std::to_string(x.m_old)
                + " " + std::to_string(ASRUtils::extract_physical_type(ASRUtils::expr_type(x.m_arg))));
        }
    }

    void visit_SubroutineCall(const SubroutineCall_t &x) {
        require(symtab_in_scope(current_symtab, x.m_name),
            "SubroutineCall::m_name '" + std::string(symbol_name(x.m_name)) + "' cannot point outside of its symbol table");
        if (check_external) {
            ASR::symbol_t *s = ASRUtils::symbol_get_past_external(x.m_name);
            if (ASR::is_a<ASR::Variable_t>(*s)) {
                ASR::Variable_t *v = ASR::down_cast<ASR::Variable_t>(s);
                require(v->m_type_declaration && ASR::is_a<ASR::Function_t>(*ASRUtils::symbol_get_past_external(v->m_type_declaration)),
                    "SubroutineCall::m_name '" + std::string(symbol_name(x.m_name)) + "' is a Variable, but does not point to Function");
                require(ASR::is_a<ASR::FunctionType_t>(*v->m_type),
                    "SubroutineCall::m_name '" + std::string(symbol_name(x.m_name)) + "' is a Variable, but the type is not FunctionType");
            } else {
                require(ASR::is_a<ASR::Function_t>(*s) ||
                        ASR::is_a<ASR::StructMethodDeclaration_t>(*s),
                    "SubroutineCall::m_name '" + std::string(symbol_name(x.m_name)) + "' must be a Function or StructMethodDeclaration.");
            }
        }

        ASR::symbol_t* asr_owner_sym = nullptr;
        if(current_symtab->asr_owner &&  ASR::is_a<ASR::symbol_t>(*current_symtab->asr_owner) ) {
            asr_owner_sym = ASR::down_cast<ASR::symbol_t>(current_symtab->asr_owner);
        }

        SymbolTable* temp_scope = current_symtab;

        if (asr_owner_sym && temp_scope->get_counter() != ASRUtils::symbol_parent_symtab(x.m_name)->get_counter() &&
            !ASR::is_a<ASR::ExternalSymbol_t>(*x.m_name) && !ASR::is_a<ASR::Variable_t>(*x.m_name)) {
            if (ASR::is_a<ASR::AssociateBlock_t>(*asr_owner_sym) || ASR::is_a<ASR::Block_t>(*asr_owner_sym)) {
                temp_scope = temp_scope->parent;
                if (temp_scope->get_counter() != ASRUtils::symbol_parent_symtab(x.m_name)->get_counter()) {
                    function_dependencies.push_back(std::string(ASRUtils::symbol_name(x.m_name)));
                }
            } else {
                function_dependencies.push_back(std::string(ASRUtils::symbol_name(x.m_name)));
            }
        }

        if( ASR::is_a<ASR::ExternalSymbol_t>(*x.m_name) ) {
            ASR::ExternalSymbol_t* x_m_name = ASR::down_cast<ASR::ExternalSymbol_t>(x.m_name);
            if( x_m_name->m_external && ASR::is_a<ASR::Module_t>(*ASRUtils::get_asr_owner(x_m_name->m_external)) ) {
                module_dependencies.push_back(std::string(x_m_name->m_module_name));
            }
        }

        verify_args(x);
    }

    void visit_AssociateBlockCall(const AssociateBlockCall_t &x) {
        require(symtab_in_scope(current_symtab, x.m_m),
            "AssociateBlockCall::m_name '" + std::string(symbol_name(x.m_m)) +
                "' cannot point outside of its symbol table");
    }

    ASR::symbol_t *get_parent_type_dt(ASR::symbol_t *dt) {
        ASR::symbol_t *parent = nullptr;
        switch (dt->type) {
            case (ASR::symbolType::Struct): {
                dt = ASRUtils::symbol_get_past_external(dt);
                ASR::Struct_t* der_type = ASR::down_cast<ASR::Struct_t>(dt);
                parent = der_type->m_parent;
                break;
            }
            default :
                require_with_loc(false,
                    "m_dt::m_v::m_type must point to a StructType type",
                    dt->base.loc);
        }
        return parent;
    }

    void visit_PointerNullConstant(const PointerNullConstant_t& x) {
        require(x.m_type != nullptr, "null() must have a type");
        if ( x.m_var_expr != nullptr ) {
            visit_expr(*x.m_var_expr);
        }
    }

    void visit_FunctionType(const FunctionType_t& x) {

        #define verify_nonscoped_ttype(ttype) non_global_symbol_visited = false; \
            visit_ttype(*ttype); \
            require(non_global_symbol_visited == false, \
                    "ASR::ttype_t in ASR::FunctionType" \
                    " cannot be tied to a scope."); \

        for( size_t i = 0; i < x.n_arg_types; i++ ) {
            verify_nonscoped_ttype(x.m_arg_types[i]);
        }
        if( x.m_return_var_type ) {
            verify_nonscoped_ttype(x.m_return_var_type);
        }
    }

    void visit_IntrinsicElementalFunction(const ASR::IntrinsicElementalFunction_t& x) {
        if( !check_external ) {
            BaseWalkVisitor<VerifyVisitor>::visit_IntrinsicElementalFunction(x);
            return ;
        }
        ASRUtils::verify_function verify_ = ASRUtils::IntrinsicElementalFunctionRegistry
            ::get_verify_function(x.m_intrinsic_id);
        LCOMPILERS_ASSERT(verify_ != nullptr);
        verify_(x, diagnostics);
        BaseWalkVisitor<VerifyVisitor>::visit_IntrinsicElementalFunction(x);
    }

    void visit_IntrinsicArrayFunction(const ASR::IntrinsicArrayFunction_t& x) {
        if( !check_external ) {
            BaseWalkVisitor<VerifyVisitor>::visit_IntrinsicArrayFunction(x);
            return ;
        }
        ASRUtils::verify_array_function verify_ = ASRUtils::IntrinsicArrayFunctionRegistry
            ::get_verify_function(x.m_arr_intrinsic_id);
        LCOMPILERS_ASSERT(verify_ != nullptr);
        verify_(x, diagnostics);
        BaseWalkVisitor<VerifyVisitor>::visit_IntrinsicArrayFunction(x);
    }

    void visit_FunctionCall(const FunctionCall_t &x) {
        require(x.m_name,
            "FunctionCall::m_name must be present");
        variable_dependencies.push_back(std::string(ASRUtils::symbol_name(x.m_name)));
        ASR::symbol_t* asr_owner_sym = nullptr;
        if(current_symtab->asr_owner &&  ASR::is_a<ASR::symbol_t>(*current_symtab->asr_owner) ) {
            asr_owner_sym = ASR::down_cast<ASR::symbol_t>(current_symtab->asr_owner);
        }

        SymbolTable* temp_scope = current_symtab;

        if (asr_owner_sym && temp_scope->get_counter() != ASRUtils::symbol_parent_symtab(x.m_name)->get_counter() &&
            !ASR::is_a<ASR::ExternalSymbol_t>(*x.m_name) && !ASR::is_a<ASR::Variable_t>(*x.m_name)) {
            if (ASR::is_a<ASR::AssociateBlock_t>(*asr_owner_sym) || ASR::is_a<ASR::Block_t>(*asr_owner_sym)) {
                temp_scope = temp_scope->parent;
                if (temp_scope->get_counter() != ASRUtils::symbol_parent_symtab(x.m_name)->get_counter()) {
                    function_dependencies.push_back(std::string(ASRUtils::symbol_name(x.m_name)));
                }
            } else {
                function_dependencies.push_back(std::string(ASRUtils::symbol_name(x.m_name)));
            }
        }
        if (_return_var_or_intent_out  && _processing_dims &&
            temp_scope->get_counter() != ASRUtils::symbol_parent_symtab(x.m_name)->get_counter() &&
            !ASR::is_a<ASR::ExternalSymbol_t>(*x.m_name)) {
            function_dependencies.push_back(std::string(ASRUtils::symbol_name(x.m_name)));
        }

        if( ASR::is_a<ASR::ExternalSymbol_t>(*x.m_name) ) {
            ASR::ExternalSymbol_t* x_m_name = ASR::down_cast<ASR::ExternalSymbol_t>(x.m_name);
            if( x_m_name->m_external && ASR::is_a<ASR::Module_t>(*ASRUtils::get_asr_owner(x_m_name->m_external)) ) {
                module_dependencies.push_back(std::string(x_m_name->m_module_name));
            }
        }

        require(symtab_in_scope(current_symtab, x.m_name),
            "FunctionCall::m_name `" + std::string(symbol_name(x.m_name)) +
            "` cannot point outside of its symbol table");
        // Check both `name` and `orig_name` that `orig_name` points
        // to GenericProcedure (if applicable), both external and non
        // external
        const ASR::symbol_t *fn = ASRUtils::symbol_get_past_external(x.m_name);
        if (check_external) {
            require(ASR::is_a<ASR::Function_t>(*fn) ||
                    (ASR::is_a<ASR::Variable_t>(*fn) &&
                    ASR::is_a<ASR::FunctionType_t>(*ASRUtils::symbol_type(fn))) ||
                    ASR::is_a<ASR::StructMethodDeclaration_t>(*fn),
                "FunctionCall::m_name must be a Function or Variable with FunctionType");
        }

        if( fn && ASR::is_a<ASR::Function_t>(*fn) ) {
            ASR::Function_t* fn_ = ASR::down_cast<ASR::Function_t>(fn);
            require(fn_->m_return_var != nullptr,
                    "FunctionCall::m_name " + std::string(fn_->m_name) +
                    " must be returning a non-void value.");
        }
        verify_args(x);
        visit_ttype(*x.m_type);
    }

    void visit_StructType(const StructType_t& x) {
        for (size_t i = 0; i < x.n_data_member_types; i++) {
            visit_ttype(*x.m_data_member_types[i]);
        }
    }

    void visit_ArrayConstructor(const ArrayConstructor_t& x) {
        require(ASRUtils::is_array(x.m_type),
            "Type of ArrayConstructor must be an array");
        BaseWalkVisitor<VerifyVisitor>::visit_ArrayConstructor(x);
    }

    void visit_ArrayConstant(const ArrayConstant_t& x) {
        require(ASRUtils::is_array(x.m_type),
            "Type of ArrayConstant must be an array");

        int64_t n_data = ASRUtils::get_fixed_size_of_array(x.m_type) * ASRUtils::extract_kind_from_ttype_t(x.m_type);
        if (ASRUtils::is_character(*x.m_type)) {
            ASR::ttype_t* t = ASRUtils::type_get_past_array(x.m_type);
            int64_t len;
            require(ASRUtils::extract_value(ASR::down_cast<ASR::String_t>(t)->m_len, len), "Constant array of strings should have constant string length");
            n_data = ASRUtils::get_fixed_size_of_array(x.m_type) * len;
        }
        require(n_data == x.m_n_data, "ArrayConstant::m_n_data must match the byte size of the array");
        visit_ttype(*x.m_type);
    }

    void visit_dimension(const dimension_t &x) {
        if (x.m_start) {
            if(check_external){
                require_with_loc(ASRUtils::is_integer(
                    *ASRUtils::expr_type(x.m_start)),
                    "Start dimension must be a signed integer", x.loc);
            }
            visit_expr(*x.m_start);
        }

        if (x.m_length) {
            if(check_external){
                require_with_loc(ASRUtils::is_integer(
                    *ASRUtils::expr_type(x.m_length)),
                    "Length dimension must be a signed integer", x.loc);
            }
            visit_expr(*x.m_length);
        }
    }

    void visit_Array(const Array_t& x) {
        require(!ASR::is_a<ASR::Allocatable_t>(*x.m_type),
            "Allocatable cannot be inside array");
        visit_ttype(*x.m_type);
        require(x.n_dims != 0, "Array type cannot have 0 dimensions.")
        require(!ASR::is_a<ASR::Array_t>(*x.m_type), "Array type cannot be nested.")
        if(ASRUtils::is_character(*x.m_type)){
            require(x.m_physical_type != ASR::FixedSizeArray,
                "Array of strings' physical type shouldn't be \"FixedSizeArray\"")
        }
        _processing_dims = true;
        for (size_t i = 0; i < x.n_dims; i++) {
            visit_dimension(x.m_dims[i]);
        }
        _processing_dims = false;
    }

    void visit_Pointer(const Pointer_t &x) {
        require(!ASR::is_a<ASR::Allocatable_t>(*x.m_type),
            "Pointer type conflicts with Allocatable type");
        if( ASR::is_a<ASR::Array_t>(*x.m_type) ) {
            ASR::Array_t* array_t = ASR::down_cast<ASR::Array_t>(x.m_type);
            for (size_t i = 0; i < array_t->n_dims; i++) {
                require(array_t->m_dims[i].m_start == nullptr &&
                        array_t->m_dims[i].m_length == nullptr,
                        "Array type in pointer must have deferred shape");
            }
        }
        visit_ttype(*x.m_type);
    }

    void visit_Allocatable(const Allocatable_t &x) {
        require(!ASR::is_a<ASR::Pointer_t>(*x.m_type) &&
                !ASR::is_a<ASR::Allocatable_t>(*x.m_type),
            "Allocatable type conflicts with Pointer type");
        ASR::dimension_t* m_dims = nullptr;
        size_t n_dims = ASRUtils::extract_dimensions_from_ttype(x.m_type, m_dims);
        for( size_t i = 0; i < n_dims; i++ ) {
            require(m_dims[i].m_length == nullptr,
                "Length of allocatable should be deferred (empty).");
        }
        visit_ttype(*x.m_type);
    }

    void visit_String(const String_t &x){
/*General Check on the length*/ 
        if(x.m_len){
            require(ASR::is_a<ASR::Integer_t>(*ASRUtils::expr_type(x.m_len)),
                "String length must be of type INTEGER,"
                "found " + 
                ASRUtils::type_to_str_fortran(ASRUtils::expr_type(x.m_len)));
        }
// Check Positive Length
        if(x.m_len && ASRUtils::is_value_constant(x.m_len)){
            int64_t len{};
            ASRUtils::is_value_constant(x.m_len, len);
            require(len >= 0,
                "String length must be length >= 0\nCurrent length is -> " + std::to_string(len));
        }
/*Check Valid String type state based on the physical type*/
        if (x.m_physical_type == DescriptorString ||
            x.m_physical_type == CChar){
            std::string type_as_str = (x.m_physical_type == DescriptorString) ? "\"DescriptorString\"" : "\"CChar\"";
            if(x.m_len){
                require(x.m_len_kind == ExpressionLength,
                    "String of physical type " +
                    type_as_str +
                    " + existing length => must have length kind of \"ExpressionLength\".")
            } else {
                require(x.m_len_kind == AssumedLength ||
                        x.m_len_kind == DeferredLength ||
                        x.m_len_kind == ImplicitLength,
                    "String of physical type " +
                    type_as_str +
                    " + non-existing length => must have length kind of"
                    " \"AssumedLength\" OR \"DeferredLength\" OR \"ImplicitLength\".")
            }
        } else {
            throw LCompilersException("PhysicalType not checked (Probably a new physical type).");
        }
/*Check if implicitLength is used correctly*/
        if(x.m_len_kind == ASR::ImplicitLength){
            require(current_expr && ASR::is_a<ASR::StringPhysicalCast_t>(*current_expr),
                "Implicit length kind must appear in StringPhysicalCast expression.");
        }
        BaseWalkVisitor<VerifyVisitor>::visit_String(x);
    }
    void visit_StringPhysicalCast(const StringPhysicalCast_t &x){
        require(x.m_type, "x.m_type cannot be nullptr");
        require(ASR::is_a<ASR::String_t>(*x.m_type), "StringPhysicalCast should be of string type");
        ASR::String_t* str = ASR::down_cast<ASR::String_t>(x.m_type);
        require(!str->m_len,
            "StringPhysicalCast return type shouldn't have length "
            "(Length should be implicit).")
        require(str->m_len_kind == ImplicitLength,
            "StringPhysicalCast expression should have length kind of \"ImplicitLength\".")
        BaseWalkVisitor<VerifyVisitor>::visit_StringPhysicalCast(x);
    }
    void visit_StringSection(const StringSection_t &x){
        require(x.m_start, "StringSection start member must be provided")
        require(x.m_end, "StringSection end member must be provided")
        require(x.m_step, "StringSection step member must be provided")
        require(ASR::is_a<ASR::String_t>(*x.m_type), "StringSection return type must be a string")
        require(ASRUtils::get_string_type(x.m_type)->m_len, "StringSection's string-return node must have length expression (NOT nullptr)")
        BaseWalkVisitor<VerifyVisitor>::visit_StringSection(x);
    }


    void visit_Allocate(const Allocate_t &x) {
        if(check_external){
            for( size_t i = 0; i < x.n_args; i++ ) {
                require(ASR::is_a<ASR::Allocatable_t>(*ASRUtils::expr_type(x.m_args[i].m_a)) ||
                        ASR::is_a<ASR::Pointer_t>(*ASRUtils::expr_type(x.m_args[i].m_a)),
                    "Allocate should only be called with  Allocatable or Pointer type inputs, found " +
                    std::string(ASRUtils::get_type_code(ASRUtils::expr_type(x.m_args[i].m_a))));
                ASR::ttype_t* alloc_arg_type = x.m_args[i].m_type;
                if ( alloc_arg_type && ASRUtils::is_struct(*alloc_arg_type) && x.m_args[i].m_sym_subclass != nullptr) {
                    require(ASR::is_a<ASR::Struct_t>(*ASRUtils::symbol_get_past_external(x.m_args[i].m_sym_subclass)),
                        "Allocate::m_sym_subclass must point to a Struct_t when the m_a member is of a type StructType");
                }
                // Check Allocating a string OR an array of string with deferred length
                // Not providing length in Allocate statement with non-deferredLength is permissible
                if(!x.m_source &&
                    ASRUtils::is_character(*ASRUtils::expr_type(x.m_args[i].m_a)) && 
                    ASRUtils::get_string_type(ASRUtils::expr_type(x.m_args[i].m_a))->m_len_kind == ASR::DeferredLength){
                    require(x.m_args[i].m_len_expr,
                        "Allocating a variable that's a string of deferred length requires providing a length to allocate with");
                }
            }

            if( x.m_source == nullptr ) {
                for( size_t i = 0; i < x.n_args; i++ ) {
                    if( ASRUtils::is_array(ASRUtils::expr_type(x.m_args[i].m_a)) ) {
                        require(x.m_args[i].n_dims > 0,
                            "Allocate for arrays should have dimensions specified, "
                            "found only array variable with no dimensions");
                    }
                }
            }
        }

        BaseWalkVisitor<VerifyVisitor>::visit_Allocate(x);
    }

    void visit_DoConcurrentLoop(const DoConcurrentLoop_t &x) {
        for ( size_t i = 0; i < x.n_local; i++ ) {
            require(ASR::is_a<ASR::Var_t>(*x.m_local[i]),
                "DoConcurrentLoop::m_local must be a Var");
        }
        for ( size_t i = 0; i < x.n_shared; i++ ) {
            require(ASR::is_a<ASR::Var_t>(*x.m_shared[i]),
                "DoConcurrentLoop::m_shared must be a Var");
        }
        BaseWalkVisitor<VerifyVisitor>::visit_DoConcurrentLoop(x);
    }

};


} // namespace ASR

bool asr_verify(const ASR::TranslationUnit_t &unit, bool check_external,
            diag::Diagnostics &diagnostics) {
    ASR::VerifyVisitor v(check_external, diagnostics);
    try {
        v.visit_TranslationUnit(unit);
    } catch (const ASRUtils::VerifyAbort &) {
        LCOMPILERS_ASSERT(diagnostics.has_error())
        return false;
    }
    return true;
}

} // namespace LCompilers
