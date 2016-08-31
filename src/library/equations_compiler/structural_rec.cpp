/*
Copyright (c) 2016 Microsoft Corporation. All rights reserved.
Released under Apache 2.0 license as described in the file LICENSE.

Author: Leonardo de Moura
*/
#include "kernel/instantiate.h"
#include "kernel/inductive/inductive.h"
#include "library/trace.h"
#include "library/constants.h"
#include "library/locals.h"
#include "library/util.h"
#include "library/app_builder.h"
#include "library/replace_visitor_with_tc.h"
#include "library/equations_compiler/equations.h"
#include "library/equations_compiler/util.h"
#include "library/equations_compiler/structural_rec.h"
#include "library/equations_compiler/elim_match.h"

namespace lean {
#define trace_struct(Code) lean_trace(name({"eqn_compiler", "structural_rec"}), type_context ctx = mk_type_context(); scope_trace_env _scope1(m_env, ctx); Code)
#define trace_struct_aux(Code) lean_trace(name({"eqn_compiler", "structural_rec"}), scope_trace_env _scope1(m_ctx.env(), m_ctx); Code)


struct structural_rec_fn {
    environment      m_env;
    options          m_opts;
    metavar_context  m_mctx;
    local_context    m_lctx;

    equations_header m_header;
    expr             m_fn_type;
    unsigned         m_arity;
    unsigned         m_arg_pos;
    buffer<unsigned> m_indices_pos;
    expr             m_motive_type;

    structural_rec_fn(environment const & env, options const & opts,
                      metavar_context const & mctx, local_context const & lctx):
        m_env(env), m_opts(opts), m_mctx(mctx), m_lctx(lctx) {
    }

    type_context mk_type_context() {
        return type_context(m_env, m_opts, m_mctx, m_lctx, transparency_mode::Semireducible);
    }

    environment const & env() const { return m_env; }
    metavar_context const & mctx() const { return m_mctx; }

    /** \brief Auxiliary object for checking whether recursive application are
        structurally smaller or not */
    struct check_rhs_fn {
        type_context & m_ctx;
        expr           m_lhs;
        expr           m_fn;
        expr           m_pattern;
        unsigned       m_arg_idx;

        check_rhs_fn(type_context & ctx, expr const & lhs, expr const & fn, expr const & pattern, unsigned arg_idx):
            m_ctx(ctx), m_lhs(lhs), m_fn(fn), m_pattern(pattern), m_arg_idx(arg_idx) {}

        bool is_constructor(expr const & e) const {
            return is_constant(e) && inductive::is_intro_rule(m_ctx.env(), const_name(e));
        }

        /** \brief Return true iff \c s is structurally smaller than \c t OR equal to \c t */
        bool is_le(expr const & s, expr const & t) {
            return m_ctx.is_def_eq(s, t) || is_lt(s, t);
        }

        /** Return true iff \c s is structurally smaller than \c t */
        bool is_lt(expr s, expr t) {
            s = m_ctx.whnf(s);
            t = m_ctx.whnf(t);
            if (is_app(s)) {
                expr const & s_fn = get_app_fn(s);
                if (!is_constructor(s_fn))
                    return is_lt(s_fn, t); // f < t ==> s := f a_1 ... a_n < t
            }
            buffer<expr> t_args;
            expr const & t_fn = get_app_args(t, t_args);
            if (!is_constructor(t_fn))
                return false;
            return std::any_of(t_args.begin(), t_args.end(),
                               [&](expr const & t_arg) { return is_le(s, t_arg); });
        }

        /** \brief Return true iff all recursive applications in \c e are structurally smaller than \c m_pattern. */
        bool check_rhs(expr const & e) {
            switch (e.kind()) {
            case expr_kind::Var:   case expr_kind::Meta:
            case expr_kind::Local: case expr_kind::Constant:
            case expr_kind::Sort:
                return true;
            case expr_kind::Macro:
                for (unsigned i = 0; i < macro_num_args(e); i++)
                    if (!check_rhs(macro_arg(e, i)))
                        return false;
                return true;
            case expr_kind::App: {
                buffer<expr> args;
                expr const & fn = get_app_args(e, args);
                if (!check_rhs(fn))
                    return false;
                for (unsigned i = 0; i < args.size(); i++)
                    if (!check_rhs(args[i]))
                        return false;
                if (is_local(fn) && mlocal_name(fn) == mlocal_name(m_fn)) {
                    /* recusive application */
                    if (m_arg_idx < args.size()) {
                        expr const & arg = args[m_arg_idx];
                        /* arg must be structurally smaller than m_pattern */
                        if (!is_lt(arg, m_pattern)) {
                            trace_struct_aux(tout() << "structural recursion on argument #" << (m_arg_idx+1)
                                             << " was not used "
                                             << "for '" << m_fn << "'\nargument #" << (m_arg_idx+1)
                                             << " in the application\n  "
                                             << e << "\nis not structurally smaller than the one occurring in "
                                             << "the equation left-hand-side\n  "
                                             << m_lhs << "\n";);
                            return false;
                        }
                    } else {
                        /* function is not fully applied */
                        trace_struct_aux(tout() << "structural recursion on argument #" << (m_arg_idx+1) << " was not used "
                                         << "for '" << m_fn << "' because of the partial application\n  "
                                         << e << "\n";);
                        return false;
                    }
                }
                return true;
            }
            case expr_kind::Let:
                if (!check_rhs(let_value(e))) {
                    return false;
                } else {
                    type_context::tmp_locals locals(m_ctx);
                    return check_rhs(instantiate(let_body(e), locals.push_local_from_let(e)));
                }
            case expr_kind::Lambda:
            case expr_kind::Pi:
                if (!check_rhs(binding_domain(e))) {
                    return false;
                } else {
                    type_context::tmp_locals locals(m_ctx);
                    return check_rhs(instantiate(binding_body(e), locals.push_local_from_binding(e)));
                }
            }
            lean_unreachable();
        }

        bool operator()(expr const & e) {
            return check_rhs(e);
        }
    };

    bool check_rhs(type_context & ctx, expr const & lhs, expr const & fn, expr pattern, unsigned arg_idx, expr const & rhs) {
        pattern = ctx.whnf(pattern);
        return check_rhs_fn(ctx, lhs, fn, pattern, arg_idx)(rhs);
    }

    bool check_eq(type_context & ctx, expr const & eqn, unsigned arg_idx) {
        unpack_eqn ue(ctx, eqn);
        buffer<expr> args;
        expr const & fn  = get_app_args(ue.lhs(), args);
        return check_rhs(ctx, ue.lhs(), fn, args[arg_idx], arg_idx, ue.rhs());
    }

    static bool depends_on_locals(expr const & e, type_context::tmp_locals const & locals) {
        return depends_on_any(e, locals.as_buffer().size(), locals.as_buffer().data());
    }

    /* Return true iff argument arg_idx is a candidate for structural recursion.
       If the argument type is an indexed family, we store the position of the
       indices (in the function being defined) in the buffer idx_positions.*/
    bool check_arg_type(type_context & ctx, unpack_eqns const & ues, unsigned arg_idx, buffer<unsigned> & idx_positions) {
        type_context::tmp_locals locals(ctx);
        /* We can only use structural recursion on arg_idx IF
           1- Type is an inductive datatype with support for the brec_on construction.
           2- Type parameters do not depend on other arguments of the function being defined. */
        expr fn      = ues.get_fn(0);
        expr fn_type = ctx.infer(fn);
        for (unsigned i = 0; i < arg_idx; i++) {
            fn_type = ctx.whnf(fn_type);
            if (!is_pi(fn_type)) throw_ill_formed_eqns();
            fn_type = instantiate(binding_body(fn_type), locals.push_local_from_binding(fn_type));
        }
        if (!is_pi(fn_type)) throw_ill_formed_eqns();
        expr arg_type = ctx.relaxed_whnf(binding_domain(fn_type));
        buffer<expr> I_args;
        expr I        = get_app_args(arg_type, I_args);
        if (is_constant(I) && !inductive::is_inductive_decl(m_env, const_name(I))) {
            trace_struct(tout() << "structural recursion on argument #" << (arg_idx+1) << " was not used "
                         << "for '" << fn << "' because type is not inductive\n  "
                         << arg_type << "\n";);
            return false;
        }
        if (!m_env.find(name(const_name(I), "brec_on"))) {
            trace_struct(tout() << "structural recursion on argument #" << (arg_idx+1) << " was not used "
                         << "for '" << fn << "' because the inductive type '" << I << "' does have brec_on recursor\n  "
                         << arg_type << "\n";);
            return false;
        }
        unsigned nindices = *inductive::get_num_indices(m_env, const_name(I));
        if (nindices > 0) {
            lean_assert(I_args.size() >= nindices);
            for (unsigned i = I_args.size() - nindices; i < I_args.size(); i++) {
                expr const & idx = I_args[i];
                if (!is_local(idx)) {
                    trace_struct(tout() << "structural recursion on argument #" << (arg_idx+1) << " was not used "
                                 << "for '" << fn << "' because the inductive type '" << I << "' is an indexed family, "
                                 << "and index #" << (i+1) << " is not a variable\n  "
                                 << arg_type << "\n";);
                    return false;
                }
                /* Index must be an argument of the function being defined */
                unsigned idx_pos = 0;
                buffer<expr> const & xs = locals.as_buffer();
                for (; idx_pos < xs.size(); idx_pos++) {
                    expr const & x = xs[idx_pos];
                    if (mlocal_name(x) == mlocal_name(idx)) {
                        break;
                    }
                }
                if (idx_pos == xs.size()) {
                    trace_struct(tout() << "structural recursion on argument #" << (arg_idx+1) << " was not used "
                                 << "for '" << fn << "' because the inductive type '" << I << "' is an indexed family, "
                                 << "and index #" << (i+1) << " is not an argument of the function being defined\n  "
                                 << arg_type << "\n";);
                    return false;
                }
                /* Index can only depend on other indices in the function being defined. */
                expr idx_type = ctx.infer(idx);
                for (unsigned j = 0; j < idx_pos; j++) {
                    bool j_is_not_index = std::find(idx_positions.begin(), idx_positions.end(), j) == idx_positions.end();
                    if (j_is_not_index && depends_on(idx_type, xs[j])) {
                        trace_struct(tout() << "structural recursion on argument #" << (arg_idx+1) << " was not used "
                                     << "for '" << fn << "' because the inductive type '" << I << "' is an indexed family, "
                                     << "and index #" << (i+1) << " depends on argument #" << (j+1) << " of '" << fn << "' "
                                     << "which is not an index of the inductive datatype\n  "
                                     << arg_type << "\n";);
                        return false;
                    }
                }
                idx_positions.push_back(idx_pos);
                /* Each index can only occur once */
                for (unsigned j = 0; j < i; j++) {
                    expr const & prev_idx = I_args[j];
                    if (mlocal_name(prev_idx) == mlocal_name(idx)) {
                        trace_struct(tout() << "structural recursion on argument #" << (arg_idx+1) << " was not used "
                                     << "for '" << fn << "' because the inductive type '" << I << "' is an indexed family, "
                                     << "and index #" << (i+1) << " and #" << (j+1) << " must be different variables\n  "
                                     << arg_type << "\n";);
                        return false;
                    }
                }
            }
        }
        for (unsigned i = 0; i < I_args.size() - nindices; i++) {
            if (depends_on_locals(I_args[i], locals)) {
                trace_struct(tout() << "structural recursion on argument #" << (arg_idx+1) << " was not used "
                             << "for '" << fn << "' because type parameter depends on previous arguments\n  "
                             << arg_type << "\n";);
                return false;
            }
        }
        return true;
    }

    /* Return true iff structural recursion can be performed on one of the arguments.
       If the result is true, then m_arg_pos will contain the position of the argument,
       and m_indices_pos the position of its indices (when the type of the
       argument is an indexed family). */
    bool find_rec_arg(type_context & ctx, unpack_eqns const & ues) {
        buffer<expr> const & eqns = ues.get_eqns_of(0);
        unsigned arity = ues.get_arity_of(0);
        for (unsigned i = 0; i < arity; i++) {
            m_indices_pos.clear();
            if (check_arg_type(ctx, ues, i, m_indices_pos)) {
                bool ok = true;
                for (expr const & eqn : eqns) {
                    if (!check_eq(ctx, eqn, i)) {
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    m_arg_pos = i;
                    return true;
                }
            }
        }
        return false;
    }

    /* Return the type of the new function.
       It also sets the m_motive_type field. */
    expr mk_new_fn_motive_types(type_context & ctx, unpack_eqns const & ues) {
        type_context::tmp_locals locals(ctx);
        expr fn        = ues.get_fn(0);
        expr fn_type   = ctx.infer(fn);
        unsigned arity = ues.get_arity_of(0);
        expr rec_arg;
        buffer<expr> other_args;
        buffer<expr> idx_args;
        for (unsigned i = 0; i < arity; i++) {
            fn_type = ctx.whnf(fn_type);
            if (!is_pi(fn_type)) throw_ill_formed_eqns();
            expr arg = locals.push_local_from_binding(fn_type);
            if (i == m_arg_pos) {
                rec_arg = arg;
            } else if (std::find(m_indices_pos.begin(), m_indices_pos.end(), i) != m_indices_pos.end()) {
                idx_args.push_back(arg);
            } else {
                other_args.push_back(arg);
            }
            fn_type  = instantiate(binding_body(fn_type), arg);
        }
        buffer<expr> I_params;
        expr I = get_app_args(ctx.infer(rec_arg), I_params);
        unsigned nindices = m_indices_pos.size();
        I_params.shrink(I_params.size() - nindices);
        expr  motive = ctx.mk_pi(other_args, fn_type);
        level u      = get_level(ctx, motive);
        motive       = ctx.mk_lambda(idx_args, ctx.mk_lambda(rec_arg, motive));
        lean_assert(is_constant(I));
        buffer<level> below_lvls;
        below_lvls.push_back(u);
        for (level const & v : const_levels(I))
            below_lvls.push_back(v);
        expr below    = mk_app(mk_constant(name(const_name(I), "below"), to_list(below_lvls)), I_params);
        m_motive_type = binding_domain(ctx.relaxed_whnf(ctx.infer(below)));
        below         = mk_app(mk_app(mk_app(below, motive), idx_args), rec_arg);
        locals.push_local("_F", below);
        return locals.mk_pi(fn_type);
    }

    struct elim_rec_apps_failed {};

    struct elim_rec_apps_fn : public replace_visitor_with_tc {
        expr                     m_fn;
        unsigned                 m_arg_pos;
        buffer<unsigned> const & m_indices_pos;
        expr                     m_F;
        expr                     m_C;

        elim_rec_apps_fn(type_context & ctx, expr const & fn,
                         unsigned arg_pos, buffer<unsigned> const & indices_pos, expr const & F, expr const & C):
            replace_visitor_with_tc(ctx),
            m_fn(fn), m_arg_pos(arg_pos), m_indices_pos(indices_pos), m_F(F), m_C(C) {}

        /** \brief Retrieve result for \c a from the below dictionary \c d. \c d is a term made of products,
            and m_C (the abstract local). */
        optional<expr> to_below(expr const & d, expr const & a, expr const & F) {
            expr const & fn = get_app_fn(d);
            if (is_constant(fn, get_prod_name())) {
                expr d_arg1 = m_ctx.whnf(app_arg(app_fn(d)));
                expr d_arg2 = m_ctx.whnf(app_arg(d));
                if (auto r = to_below(d_arg1, a, mk_pr1(m_ctx, F)))
                    return r;
                else if (auto r = to_below(d_arg2, a, mk_pr2(m_ctx, F)))
                    return r;
                else
                    return none_expr();
            } else if (is_local(fn)) {
                if (mlocal_name(m_C) == mlocal_name(fn) && m_ctx.is_def_eq(app_arg(d), a))
                    return some_expr(F);
                return none_expr();
            } else if (is_pi(d)) {
                if (is_app(a)) {
                    expr new_d = m_ctx.whnf(instantiate(binding_body(d), app_arg(a)));
                    return to_below(new_d, a, mk_app(F, app_arg(a)));
                } else {
                    return none_expr();
                }
            } else {
                return none_expr();
            }
        }

        bool is_index_pos(unsigned idx) const {
            return std::find(m_indices_pos.begin(), m_indices_pos.end(), idx) != m_indices_pos.end();
        }

        expr elim(buffer<expr> const & args, tag g) {
            /* Replace motives with abstract one m_C.
               We use the abstract motive m_C as "marker". */
            buffer<expr> below_args;
            expr const & below_cnst = get_app_args(m_ctx.infer(m_F), below_args);
            unsigned nindices = m_indices_pos.size();
            below_args[below_args.size() - 1 - 1 /* major */ - nindices] = m_C;
            expr abst_below   = mk_app(below_cnst, below_args);
            expr below_dict   = m_ctx.whnf(abst_below);
            expr rec_arg      = m_ctx.whnf(args[m_arg_pos]);
            if (optional<expr> b = to_below(below_dict, rec_arg, m_F)) {
                expr r = *b;
                for (unsigned i = 0; i < args.size(); i++) {
                    if (i != m_arg_pos && !is_index_pos(i))
                        r = mk_app(r, args[i], g);
                }
                return r;
            } else {
                throw elim_rec_apps_failed();
            }
        }

        virtual expr visit_local(expr const & e) {
            if (mlocal_name(e) == mlocal_name(m_fn)) {
                /* unexpected occurrence of recursive function */
                throw elim_rec_apps_failed();
            }
            return e;
        }

        virtual expr visit_app(expr const & e) {
            expr const & fn = get_app_fn(e);
            if (is_local(fn) && mlocal_name(fn) == mlocal_name(m_fn)) {
                buffer<expr> args;
                get_app_args(e, args);
                if (m_arg_pos >= args.size()) throw elim_rec_apps_failed();
                buffer<expr> new_args;
                for (expr const & arg : args)
                    new_args.push_back(visit(arg));
                return elim(new_args, e.get_tag());
            } else {
                return replace_visitor_with_tc::visit_app(e);
            }
        }
    };

    void update_eqs(type_context & ctx, unpack_eqns & ues, expr const & fn, expr const & new_fn) {
        /* C is a temporary "abstract" motive, we use it to access the "brec_on dictionary".
           The "brec_on dictionary is an element of type below, and it is the last argument of the new function. */
        expr C = mk_local(mk_fresh_name(), "_C", m_motive_type, binder_info());
        buffer<expr> & eqns = ues.get_eqns_of(0);
        for (expr & eqn : eqns) {
            unpack_eqn ue(ctx, eqn);
            expr lhs = ue.lhs();
            expr rhs = ue.rhs();
            buffer<expr> lhs_args;
            get_app_args(lhs, lhs_args);
            expr new_lhs = mk_app(new_fn, lhs_args);
            expr type    = ctx.whnf(ctx.infer(new_lhs));
            lean_assert(is_pi(type));
            expr F       = ue.add_var(binding_name(type), binding_domain(type));
            new_lhs      = mk_app(new_lhs, F);
            ue.lhs()     = new_lhs;
            ue.rhs()     = elim_rec_apps_fn(ctx, fn, m_arg_pos, m_indices_pos, F, C)(rhs);
            eqn          = ue.repack();
        }
    }

    optional<expr> elim_recursion(expr const & e) {
        type_context ctx = mk_type_context();
        unpack_eqns ues(ctx, e);
        if (ues.get_num_fns() != 1) {
            trace_struct(tout() << "structural recursion is not supported for mutually recursive functions:";
                         for (unsigned i = 0; i < ues.get_num_fns(); i++)
                             tout() << " " << ues.get_fn(i);
                         tout() << "\n";);
            return none_expr();
        }
        m_fn_type = ctx.infer(ues.get_fn(0));
        m_arity   = ues.get_arity_of(0);
        if (!find_rec_arg(ctx, ues)) return none_expr();
        expr fn = ues.get_fn(0);
        trace_struct(tout() << "using structural recursion on argument #" << (m_arg_pos+1) <<
                     " for '" << fn << "'\n";);
        expr new_fn_type = mk_new_fn_motive_types(ctx, ues);
        trace_struct(
            tout() << "\n";
            tout() << "new function type: " << new_fn_type << "\n";
            tout() << "motive type:       " << m_motive_type << "\n";);
        expr new_fn = ues.update_fn_type(0, new_fn_type);
        try {
            update_eqs(ctx, ues, fn, new_fn);
        } catch (elim_rec_apps_failed &) {
            trace_struct(tout() << "failed to compile equations/match using structural recursion, "
                         << "when creating new set of equations\n";);
            return none_expr();
        }
        expr new_eqns = ues.repack();
        lean_trace("eqn_compiler", tout() << "using structural recursion:\n" << new_eqns << "\n";);
        m_mctx = ctx.mctx();
        return some_expr(new_eqns);
    }

    expr whnf_upto_below(type_context & ctx, name const & I_name, expr const & below_type) {
        name below_name(I_name, "below");
        return ctx.whnf_pred(below_type, [&](expr const & e) {
                expr const & fn = get_app_fn(e);
                return !is_constant(fn) || const_name(fn) != below_name;
            });
    }

    bool is_index_pos(unsigned idx) const {
        return std::find(m_indices_pos.begin(), m_indices_pos.end(), idx) != m_indices_pos.end();
    }

    expr mk_function(expr const & aux_fn) {
        type_context ctx = mk_type_context();
        type_context::tmp_locals locals(ctx);
        buffer<expr> fn_args;
        expr aux_fn_type = ctx.infer(aux_fn);
        for (unsigned i = 0; i < m_arity + 1 /* below argument */; i++) {
            aux_fn_type = ctx.relaxed_whnf(aux_fn_type);
            lean_assert(is_pi(aux_fn_type));
            expr arg = locals.push_local_from_binding(aux_fn_type);
            if (i < m_arity) fn_args.push_back(arg);
            aux_fn_type = instantiate(binding_body(aux_fn_type), arg);
        }
        buffer<expr> const & aux_fn_args = locals.as_buffer();
        unsigned nindices = m_indices_pos.size();
        expr rec_arg      = aux_fn_args[m_arg_pos];
        expr rec_arg_type = ctx.relaxed_whnf(ctx.infer(rec_arg));
        buffer<expr> I_args;
        expr const & I    = get_app_args(rec_arg_type, I_args);
        name I_name       = const_name(I);
        unsigned nparams  = I_args.size() - nindices;
        expr below_arg    = aux_fn_args.back();
        expr below_type   = whnf_upto_below(ctx, I_name, ctx.infer(below_arg));
        buffer<expr> below_args;
        expr below        = get_app_args(below_type, below_args);
        expr motive       = below_args[nparams];
        expr brec_on_fn   = mk_constant(name(I_name, "brec_on"), const_levels(below));
        buffer<expr> brec_on_args;
        buffer<expr> F_domain; /* domain for F argument for brec_on */
        brec_on_args.append(nparams, I_args.data());
        brec_on_args.push_back(motive);
        for (unsigned idx_pos : m_indices_pos) {
            brec_on_args.push_back(aux_fn_args[idx_pos]);
            F_domain.push_back(aux_fn_args[idx_pos]);
        }
        brec_on_args.push_back(rec_arg);
        F_domain.push_back(rec_arg);
        F_domain.push_back(below_arg);
        buffer<expr> extra_args;
        for (unsigned i = 0; i < fn_args.size(); i++) {
            if (i != m_arg_pos && !is_index_pos(i)) {
                F_domain.push_back(aux_fn_args[i]);
                extra_args.push_back(aux_fn_args[i]);
            }
        }
        expr F = ctx.mk_lambda(F_domain, mk_app(aux_fn, aux_fn_args));
        brec_on_args.push_back(F);
        expr new_fn = ctx.mk_lambda(fn_args, mk_app(mk_app(brec_on_fn, brec_on_args), extra_args));
        lean_trace("eqn_compiler", tout() << "result:\n" << new_fn << "\ntype:\n" << ctx.infer(new_fn) << "\n";);
        if (m_header.m_is_meta) {
            /* We don't create auxiliary definitions for meta-definitions because we don't create lemmas
               for them. */
            return new_fn;
        } else {
            expr r;
            std::tie(m_env, r) = mk_aux_definition(m_env, m_mctx, m_lctx, m_header.m_is_private, head(m_header.m_fn_names),
                                                   m_fn_type, new_fn);
            return r;
        }
    }

    void mk_lemmas(expr const & fn, expr const & aux_fn, list<expr_pair> const & lemmas) {
        for (expr_pair const & p : lemmas) {
            expr type, proof;
            std::tie(type, proof) = p;
            // TODO(Leo)
        }
    }

    optional<expr> operator()(expr const & eqns) {
        m_header = get_equations_header(eqns);
        auto new_eqns = elim_recursion(eqns);
        if (!new_eqns) return none_expr();
        elim_match_result R = elim_match(m_env, m_opts, m_mctx, m_lctx, *new_eqns);
        expr fn = mk_function(R.m_fn);
        if (m_header.m_lemmas) {
            lean_assert(!m_header.m_is_meta);
            mk_lemmas(fn, R.m_fn, R.m_lemmas);
        }
        return some_expr(fn);
    }
};

optional<expr> try_structural_rec(environment & env, options const & opts, metavar_context & mctx,
                                  local_context const & lctx, expr const & eqns) {
    structural_rec_fn F(env, opts, mctx, lctx);
    if (auto r = F(eqns)) {
        env  = F.env();
        mctx = F.mctx();
        return r;
    } else {
        return none_expr();
    }
}

void initialize_structural_rec() {
    register_trace_class({"eqn_compiler", "structural_rec"});
}
void finalize_structural_rec() {}
}