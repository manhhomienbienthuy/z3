/*++
Copyright (c) 2024 Microsoft Corporation

Module Name:

    bv_sls.cpp

Abstract:

    A Stochastic Local Search (SLS) engine
    Uses invertibility conditions, 
    interval annotations
    don't care annotations

Author:

    Nikolaj Bjorner (nbjorner) 2024-02-07
    
--*/

#include "ast/sls/bv_sls.h"
#include "ast/ast_pp.h"
#include "ast/ast_ll_pp.h"
#include "params/sls_params.hpp"

namespace bv {

    sls::sls(ast_manager& m): 
        m(m), 
        bv(m),
        m_terms(m),
        m_eval(m)
    {}

    void sls::init() {
        m_terms.init(); 
    }

    void sls::init_eval(std::function<bool(expr*, unsigned)>& eval) {
        m_eval.init_eval(m_terms.assertions(), eval);
        m_eval.init_fixed(m_terms.assertions());
        init_repair();
    }

    void sls::init_repair() {
        m_repair_down.reset();
        m_repair_up.reset();
        for (auto* e : m_terms.assertions()) {
            if (!m_eval.bval0(e)) {
                m_eval.set(e, true);
                m_repair_down.insert(e->get_id());
            }
        }
        for (app* t : m_terms.terms()) 
            if (t && !eval_is_correct(t))
                m_repair_down.insert(t->get_id());
    }

    void sls::reinit_eval() {
        std::function<bool(expr*, unsigned)> eval = [&](expr* e, unsigned i) {
            auto should_keep = [&]() {
                return m_rand() % 100 >= 98;
            };
            if (m.is_bool(e)) {
                if (m_eval.is_fixed0(e) || should_keep())
                    return m_eval.bval0(e);
            }
            else if (bv.is_bv(e)) {
                auto& w = m_eval.wval0(e);
                if (w.get(w.fixed, i) || should_keep())
                    return w.get(w.bits, i);                
            }
            return m_rand() % 2 == 0;
        };
        m_eval.init_eval(m_terms.assertions(), eval);
        init_repair();
    }

    std::pair<bool, app*> sls::next_to_repair() {
        app* e = nullptr;
        if (!m_repair_down.empty()) {
            unsigned index = m_rand(m_repair_down.size());
            e = m_terms.term(m_repair_down.elem_at(index));
        }
        else if (!m_repair_up.empty()) {
            unsigned index = m_rand(m_repair_up.size());
            e = m_terms.term(m_repair_up.elem_at(index));
        }
        return { !m_repair_down.empty(), e };
    }

    lbool sls::search() {
        // init and init_eval were invoked
        unsigned n = 0;  
        for (; n++ < m_config.m_max_repairs && m.inc(); ) {
            ++m_stats.m_moves;
            auto [down, e] = next_to_repair();
            if (!e)
                return l_true;
            bool is_correct = eval_is_correct(e);
            IF_VERBOSE(20, verbose_stream() << (down ? "d #" : "u #")
                       << e->get_id() << ": "
                       << mk_bounded_pp(e, m, 1) << " ";
                       if (bv.is_bv(e)) verbose_stream() << m_eval.wval0(e) << " ";
                       if (m.is_bool(e)) verbose_stream() << m_eval.bval0(e) << " ";
                       verbose_stream() << (is_correct?"C":"U") << "\n");
            if (is_correct) {
                if (down)
                    m_repair_down.remove(e->get_id());
                else
                    m_repair_up.remove(e->get_id());
            }
            else if (down) 
                try_repair_down(e);            
            else
                try_repair_up(e);
        }
        return l_undef;
    }

    void sls::trace() {
        IF_VERBOSE(2, verbose_stream()
            << "(bvsls :restarts " << m_stats.m_restarts
            << " :repair-down " << m_repair_down.size()
            << " :repair-up " << m_repair_up.size() << ")\n");
    }

    lbool sls::operator()() {
        lbool res = l_undef;
        m_stats.reset();
        m_stats.m_restarts = 0;
        do {
            res = search();
            if (res != l_undef)
                break;
            trace();
            reinit_eval();
        } 
        while (m.inc() && m_stats.m_restarts++ < m_config.m_max_restarts);

        return res;
    }

    void sls::try_repair_down(app* e) {
        unsigned n = e->get_num_args();
        if (n > 0) {
            unsigned s = m_rand(n);
            for (unsigned i = 0; i < n; ++i)
                if (try_repair_down(e, (i + s) % n))
                    return;
        }
        m_repair_down.remove(e->get_id());
        m_repair_up.insert(e->get_id());
    }

    bool sls::try_repair_down(app* e, unsigned i) {
        expr* child = e->get_arg(i);
        bool was_repaired = m_eval.try_repair(e, i);
        if (was_repaired) {
            m_repair_down.insert(child->get_id());
            for (auto p : m_terms.parents(child))
                m_repair_up.insert(p->get_id());            
        }
        return was_repaired;
    }

    void sls::try_repair_up(app* e) {
        m_repair_up.remove(e->get_id());
        if (m_terms.is_assertion(e)) {
            m_repair_down.insert(e->get_id());
        }
        else {
            m_eval.repair_up(e);
            for (auto p : m_terms.parents(e))
                m_repair_up.insert(p->get_id());
        }
    }

    bool sls::eval_is_correct(app* e) {
        if (!m_eval.can_eval1(e))
            return false;
        if (m.is_bool(e))
            return m_eval.bval0(e) == m_eval.bval1(e);
        if (bv.is_bv(e))
            return m_eval.wval0(e).eq(m_eval.wval1(e));
        UNREACHABLE();
        return false;
    }

    model_ref sls::get_model() {
        model_ref mdl = alloc(model, m);
        auto& terms = m_eval.sort_assertions(m_terms.assertions());
        for (expr* e : terms) {
            if (!is_uninterp_const(e))
                continue;
            auto f = to_app(e)->get_decl();
            if (m.is_bool(e))
                mdl->register_decl(f, m.mk_bool_val(m_eval.bval0(e)));
            else if (bv.is_bv(e)) {
                auto const& v = m_eval.wval0(e);
                rational n;
                v.get_value(v.bits, n);
                mdl->register_decl(f, bv.mk_numeral(n, v.bw));
            }
        }
        terms.reset();
        return mdl;
    }

    std::ostream& sls::display(std::ostream& out) { 
        auto& terms = m_eval.sort_assertions(m_terms.assertions());
        for (expr* e : terms) {
            out << e->get_id() << ": " << mk_bounded_pp(e, m, 1) << " ";
            if (m_eval.is_fixed0(e))
                out << "f ";
            if (m_repair_down.contains(e->get_id()))
                out << "d ";
            if (m_repair_up.contains(e->get_id()))
                out << "u ";
            if (bv.is_bv(e))
                out << m_eval.wval0(e);
            else if (m.is_bool(e))
                out << (m_eval.bval0(e)?"T":"F");
            out << "\n";
        }
        terms.reset();
        return out; 
    }

    void sls::updt_params(params_ref const& _p) {
        sls_params p(_p);
        m_config.m_max_restarts = p.max_restarts();
        m_rand.set_seed(p.random_seed());
    }
}
