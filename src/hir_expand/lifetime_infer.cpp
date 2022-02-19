/*
 * MRustC - Rust Compiler
 * - By John Hodge (Mutabah/thePowersGang)
 *
 * hir_expand/lifetime_infer.cpp
 * - Infer and check lifetime annotations
 */
#include <hir/visitor.hpp>
#include <hir/expr.hpp>
#include <hir_typeck/static.hpp>
#include <algorithm>
#include <hir/expr_state.hpp>
#include "main_bindings.hpp"


namespace {
    struct LifetimeInferState
    {
        TAGGED_UNION(LocalLifetimeData, Composite,
        (Composite, std::vector<HIR::LifetimeRef>),
        (PatternBinding, struct {
            const HIR::ExprNode*    borrow_point;
            const HIR::ExprNode*    value;
            const HIR::PatternBinding* pat;
            }),
        (Node, struct {
            const HIR::ExprNode* borrow_point;
            const HIR::ExprNode* value;
            })
        );
        struct LocalLifetime
        {
            Span    borrow_span;
            LocalLifetimeData   data;
        };
        struct IvarLifetime
        {
            Span    sp;
            /// The final assigned lifetime (not an ivar)
            HIR::LifetimeRef    known;

            /// Lifetimes that become this ivar
            std::vector<HIR::LifetimeRef>   sources;
            /// Lifetimes this ivar becomes
            std::vector<HIR::LifetimeRef>   destinations;
            /// Lifetimes that must be unified with this ivar
            std::vector<HIR::LifetimeRef>   equals;

            IvarLifetime(const Span& sp): sp(sp) {}
            bool is_known() const {
                return this->known.binding != HIR::LifetimeRef::UNKNOWN;
            }
        };

        const StaticTraitResolve& m_resolve;
        std::vector<LocalLifetime>  m_locals;
        std::vector<IvarLifetime>   m_ivars;

        LifetimeInferState(const StaticTraitResolve& resolve)
            : m_resolve(resolve)
        {}

    private:
        HIR::LifetimeRef allocate_local(const Span& sp, LocalLifetimeData v) {
            auto idx = m_locals.size();
            m_locals.push_back(LocalLifetime { sp, std::move(v) });
            assert(idx < (HIR::LifetimeRef::MAX_LOCAL - 0x1'0000));
            return HIR::LifetimeRef( 0x1'0000 + idx );
        }
    public:
        HIR::LifetimeRef allocate_local(const Span& sp, std::vector<HIR::LifetimeRef> sources) {
            return allocate_local(sp, LocalLifetimeData::make_Composite(std::move(sources)));
        }
        HIR::LifetimeRef allocate_local(const Span& sp, const HIR::ExprNode& borrow_point, const HIR::ExprNode& value, const HIR::PatternBinding& pb) {
            return allocate_local(sp, LocalLifetimeData::make_PatternBinding({ &borrow_point, &value, &pb }));
        }
        HIR::LifetimeRef allocate_local(const HIR::ExprNode& borrow_point, const HIR::ExprNode& value) {
            return allocate_local(borrow_point.span(), LocalLifetimeData::Data_Node { &borrow_point, &value });
        }
        const LocalLifetime* opt_local(const Span& sp, const HIR::LifetimeRef& lft) const {
            if( 0x1'0000 <= lft.binding && lft.binding < HIR::LifetimeRef::MAX_LOCAL ) {
                auto idx = lft.binding - 0x1'0000;
                ASSERT_BUG(sp, idx < m_locals.size(), "Local lifetime index out of range - " << lft);
                return &m_locals[idx];
            }
            else {
                return nullptr;
            }
        }

        HIR::LifetimeRef allocate_ivar(const Span& sp) {
            auto idx = m_ivars.size();
            m_ivars.push_back(IvarLifetime(sp));
            assert(idx < (0x10'0000 -  HIR::LifetimeRef::MAX_LOCAL));
            auto rv = HIR::LifetimeRef( HIR::LifetimeRef::MAX_LOCAL + idx );
            DEBUG("Allocate " << rv);
            return rv;
        }
        IvarLifetime* opt_ivar(const Span& sp, const HIR::LifetimeRef& lft) {
            if( HIR::LifetimeRef::MAX_LOCAL <= lft.binding ) {
                auto idx = lft.binding - HIR::LifetimeRef::MAX_LOCAL;
                ASSERT_BUG(sp, idx < m_ivars.size(), "IVar lifetime index out of range - " << lft);
                return &m_ivars[idx];
            }
            else {
                return nullptr;
            }
        }
        const IvarLifetime* opt_ivar(const Span& sp, const HIR::LifetimeRef& lft) const {
            if( HIR::LifetimeRef::MAX_LOCAL <= lft.binding ) {
                auto idx = lft.binding - HIR::LifetimeRef::MAX_LOCAL;
                ASSERT_BUG(sp, idx < m_ivars.size(), "IVar lifetime index out of range - " << lft);
                return &m_ivars[idx];
            }
            else {
                return nullptr;
            }
        }



        HIR::LifetimeRef get_lft_for_iv(const IvarLifetime& iv) const {
            return HIR::LifetimeRef( HIR::LifetimeRef::MAX_LOCAL + (&iv - m_ivars.data()) );
        }
        HIR::LifetimeRef get_final_lft(Span sp, HIR::LifetimeRef lft) const {
            while( const auto* s = opt_ivar(sp, lft) ) {
                // TODO: Detect and panic on infinite loops
                if( !s->is_known() ) {
                    return lft;
                }
                sp = s->sp;
                lft = s->known;
            }
            return lft;
        }
        void dump() {
            for(const auto& l : m_locals) {
                auto lft = HIR::LifetimeRef( 0x1'0000 + (&l - m_locals.data()) );
                TU_MATCH_HDRA( (l.data), { )
                TU_ARMA(Composite, le) {
                    DEBUG(lft << " := {" << le << "}");
                    }
                TU_ARMA(PatternBinding, le) {
                    DEBUG(lft << " := PB " << le.pat << " (" << le.value << " @ " << le.borrow_point << ")");
                    }
                TU_ARMA(Node, le) {
                    DEBUG(lft << " := Node " << le.value << " @ " << le.borrow_point);
                    }
                }
            }
            for(const auto& iv : m_ivars) {
                auto lft = get_lft_for_iv(iv);
                if( iv.is_known() ) {
                    DEBUG(lft << " = " << iv.known << " to=[" << iv.destinations << "]");
                }
                else {
                    DEBUG(lft << " -- to=[" << iv.destinations << "], from=[" << iv.sources << "]");
                }
            }
        }

        bool iterate_lft_bounds(std::function<bool(const HIR::GenericBound::Data_Lifetime&)> cb) const
        {
            if( m_resolve.m_impl_generics ) {
                for(const auto& b : m_resolve.m_impl_generics->m_bounds) {
                    if( const auto* be = b.opt_Lifetime() )
                        if( cb(*be) )
                            return true;
                }
            }
            if( m_resolve.m_item_generics ) {
                for(const auto& b : m_resolve.m_item_generics->m_bounds) {
                    if( const auto* be = b.opt_Lifetime() )
                        if( cb(*be) )
                            return true;
                }
            }
            return false;
        }

        /// Check that `rhs` is valid for `lhs` (assignment ordering)
        /// Stores the root RHS lifetimes that failed bounds in `fails`
        bool check_liftimes(const Span& sp, const HIR::LifetimeRef& lhs, const HIR::LifetimeRef& rhs, std::vector<HIR::LifetimeRef>& fails) const
        {
            //TRACE_FUNCTION_F(lhs << " = " << rhs);
            assert( !opt_ivar(sp, lhs) );
            assert( !opt_ivar(sp, rhs) );

            // Identical lifetimes: valid
            if( lhs == rhs ) {
                return true;
            }
            // 'static outlives everything
            if( rhs.binding == HIR::LifetimeRef::STATIC ) {
                return true;
            }

            // Source composite local: Valid if all entries are valid
            if( const auto* r_l = opt_local(sp, rhs) ) {
                if( const auto* r_le = r_l->data.opt_Composite() ) {
                    DEBUG("RHS composite: all");
                    bool rv = true;
                    for(const auto& inner : *r_le) {
                        rv &= this->check_liftimes(sp, lhs, inner, fails);
                    }
                    return rv;
                }
            }
            // Destination composite local: Valid if any entry is valid
            if( const auto* l_l = opt_local(sp, lhs) ) {
                if( const auto* l_le = l_l->data.opt_Composite() ) {
                    DEBUG("LHS composite: any");
                    std::vector<HIR::LifetimeRef>   tmp;
                    for(const auto& inner : *l_le) {
                        if( this->check_liftimes(sp, inner, rhs, tmp) )
                            return true;
                    }
                    fails.push_back(rhs);
                    return false;
                }
            }

            if( lhs.is_param() ) {
                if( rhs.is_param() ) {
                    // If RHS is an impl and LHS is a method, then good
                    if( (lhs.binding >> 8) == 1 && (rhs.binding >> 8) == 0 ) {
                        return true;
                    }
                    // Check for an outlives relationship
                    bool rv = iterate_lft_bounds([&](const HIR::GenericBound::Data_Lifetime& b)->bool {
                        if( b.test == rhs ) {
                            if( b.valid_for == lhs /*check_lifetimes(sp, lhs, b.valid_for)*/ ) {
                                return true;
                            }
                        }
                        return false;
                        });
                    if( !rv ) {
                        fails.push_back(rhs);
                    }
                    return rv;
                }
                else if( opt_local(sp, rhs) ) {
                    DEBUG("param from local");
                    fails.push_back(rhs);
                    return false;
                }
                else {
                    BUG(sp, "Unknown lft - " << rhs);
                }
            }
            else if( lhs.binding == HIR::LifetimeRef::STATIC ) {
                if( rhs.is_param() ) {
                    // Check for an outlives relationship (this param must be bounded to `'static`)
                    bool rv = iterate_lft_bounds([&](const HIR::GenericBound::Data_Lifetime& b)->bool {
                        if( b.test == rhs ) {
                            if( b.valid_for == lhs /*check_lifetimes(sp, lhs, b.valid_for)*/ ) {
                                return true;
                            }
                        }
                        return false;
                        });
                    if( !rv ) {
                        fails.push_back(rhs);
                    }
                    return rv;
                }
                else if( opt_local(sp, rhs) ) {
                    DEBUG("static from local");
                    fails.push_back(rhs);
                    return false;
                }
                else {
                    BUG(sp, "Unknown lft - " << rhs);
                }
            }
            else if( const auto* lhs_l = opt_local(sp, lhs) ) {
                assert(!lhs_l->data.is_Composite());

                if( rhs.is_param() ) {
                    // Invalid
                    fails.push_back(rhs);
                    return false;
                }
                else if( const auto* rhs_l = opt_local(sp, rhs) ) {
                    // Only valid if the source is identical? Or if the source's scope is longer than the destination
                    fails.push_back(rhs);
                    return false;
                }
                else {
                    BUG(sp, "Unknown lft - " << rhs);
                }
            }
            else {
                BUG(sp, "Unknown lifetime " << lhs);
            }
        }
        void ensure_outlives(const Span& sp, const HIR::LifetimeRef& lhs, const HIR::LifetimeRef& rhs) const
        {
            DEBUG(lhs << " = " << rhs);
            std::vector<HIR::LifetimeRef>   failed_bounds;
            if( !check_liftimes(sp, lhs, rhs, failed_bounds) )
            {
                ASSERT_BUG(sp, !failed_bounds.empty(), "");
                for(const HIR::LifetimeRef& b : failed_bounds)
                {
                    if(const auto* l = this->opt_local(sp, b)) {
                        TU_MATCH_HDRA( (l->data), { )
                        TU_ARMA(Composite, le) {
                            }
                        TU_ARMA(PatternBinding, le) {
                            NOTE(l->borrow_span, "Pattern binding @ " << le.pat);
                            NOTE(le.value->span(), " borrow of value here");
                            NOTE(le.borrow_point->span(), " borrowed here");
                            }
                        TU_ARMA(Node, le) {
                            NOTE(le.value->span(), "Borrow of value here");
                            NOTE(le.borrow_point->span(), " borrowed here");
                            }
                        }
                    }
                }
                ERROR(sp, E0000, "Lifetime bound " << lhs << " : " << rhs << " failed - [" << failed_bounds << "]");
            }
        }
    };
    class ExprVisitor_Enumerate: public HIR::ExprVisitorDef
    {
        const StaticTraitResolve&   m_resolve;
        const ::HIR::Function::args_t&  m_args;
        const HIR::TypeRef& m_real_ret_type;
        HIR::TypeRef m_ret_ty;

        LifetimeInferState& m_state;
        const std::vector<HIR::TypeRef>*    m_binding_types_ptr;

        std::vector<const HIR::ExprNode_Loop*>  m_loops;
        std::vector<const HIR::TypeRef*>    m_returns;

    public:
        ExprVisitor_Enumerate(const StaticTraitResolve& resolve, const ::HIR::Function::args_t& args, const HIR::TypeRef& ret_ty, LifetimeInferState& state)
            : m_resolve(resolve)
            , m_args(args)
            , m_real_ret_type(ret_ty)
            , m_state(state)
        {
        }

        void visit_root(HIR::ExprPtr& ep) {

            // Translate the return type's erased types
            const auto& sp = ep->span();
            DEBUG("m_real_ret_type = " << m_real_ret_type);
            m_ret_ty = clone_ty_with(sp, m_real_ret_type, [&](const HIR::TypeRef& tpl, HIR::TypeRef& rv)->bool {
                if( const auto* e = tpl.data().opt_ErasedType() )
                {
                    ASSERT_BUG(sp, e->m_index < ep.m_erased_types.size(),
                        "Erased type index OOB - " << e->m_origin << " " << e->m_index << " >= " << ep.m_erased_types.size());
                    rv = ep.m_erased_types[e->m_index].clone();
                    this->equate_type_lifetimes(sp, e->m_lifetime, rv);
                    return true;
                }
                return false;
                });
            DEBUG("m_ret_ty = " << m_ret_ty);

            for(auto& ty : ep.m_bindings) {
                this->visit_type(ty);
            }
            this->m_binding_types_ptr = &ep.m_bindings;

            this->visit_type(ep->m_res_type);
            ep->visit(*this);
            this->equate_types(ep->m_span, m_ret_ty, ep->m_res_type);

            m_binding_types_ptr = nullptr;
        }

    private:
        const HIR::TypeRef& get_local_var_ty(const Span& sp, size_t binding) const {
            return m_binding_types_ptr->at(binding);
        }

        void equate_lifetimes(const Span& sp, const HIR::LifetimeRef& lhs, const HIR::LifetimeRef& rhs) {
            ASSERT_BUG(sp, lhs != HIR::LifetimeRef() && lhs.binding != HIR::LifetimeRef::INFER, "Unspecified lifetime - " << lhs);
            ASSERT_BUG(sp, rhs != HIR::LifetimeRef() && rhs.binding != HIR::LifetimeRef::INFER, "Unspecified lifetime - " << rhs);
            if( lhs.is_param() && (lhs.binding >> 8) == 3 ) {
                BUG(sp, "Encountered HRL - " << lhs);
            }
            if( rhs.is_param() && (rhs.binding >> 8) == 3 ) {
                BUG(sp, "Encountered HRL - " << rhs);
            }

            if(lhs == rhs) {
                return ;
            }
            if( auto* iv = m_state.opt_ivar(sp, lhs) ) {
                iv->sources.push_back(rhs);
            }
            if( auto* iv = m_state.opt_ivar(sp, rhs) ) {
                iv->destinations.push_back(lhs);
            }
            if( !m_state.opt_ivar(sp, lhs) && !m_state.opt_ivar(sp, rhs) ) {
                m_state.ensure_outlives(sp, lhs, rhs);
            }
        }

        void equate_node_types(const HIR::ExprNode& lhs, const HIR::ExprNodeP& rhs) {
            assert(rhs);
            this->equate_types(rhs->span(), lhs.m_res_type, rhs->m_res_type);
        }
        void equate_pps(const Span& sp, const HIR::PathParams& lhs, const HIR::PathParams& rhs) {
            ASSERT_BUG(sp, lhs.m_lifetimes.size() == rhs.m_lifetimes.size(), "");
            for(size_t i = 0; i < lhs.m_lifetimes.size(); i ++) {
                equate_lifetimes(sp, lhs.m_lifetimes[i], rhs.m_lifetimes[i]);
            }
            ASSERT_BUG(sp, lhs.m_types.size() == rhs.m_types.size(), "");
            for(size_t i = 0; i < lhs.m_types.size(); i ++) {
                equate_types(sp, lhs.m_types[i], rhs.m_types[i]);
            }
            // Ignore values
        }
        void equate_traitpath(const Span& sp, const HIR::TraitPath& lhs, const HIR::TraitPath& rhs) {
            auto pp_l = lhs.m_hrls ? lhs.m_hrls->make_empty_params(true) : HIR::PathParams();
            auto pp_r = rhs.m_hrls ? rhs.m_hrls->make_empty_params(true) : HIR::PathParams();
            visit_path_params(pp_l);
            visit_path_params(pp_r);
            auto ms_l = MonomorphHrlsOnly(pp_l);
            auto ms_r = MonomorphHrlsOnly(pp_r);
            equate_pps(sp, ms_l.monomorph_path_params(sp, lhs.m_path.m_params, false), ms_r.monomorph_path_params(sp, rhs.m_path.m_params, false));
            ASSERT_BUG(sp, lhs.m_type_bounds.size() == rhs.m_type_bounds.size(), "");
            ASSERT_BUG(sp, lhs.m_trait_bounds.size() == rhs.m_trait_bounds.size(), "");
            for(const auto& l : lhs.m_type_bounds)
            {
                const auto& r = rhs.m_type_bounds.at(l.first);
                equate_types(sp, ms_l.monomorph_type(sp, l.second.type, false), ms_r.monomorph_type(sp, r.type, false));
            }
            for(const auto& l : lhs.m_trait_bounds)
            {
                const auto& r = rhs.m_trait_bounds.at(l.first);
                for(size_t i = 0; i < l.second.traits.size(); i++)
                {
                    equate_traitpath(sp, ms_l.monomorph_traitpath(sp, l.second.traits[i], false), ms_r.monomorph_traitpath(sp, r.traits[i], false));
                }
            }
        }

        void equate_types(const Span& sp, const HIR::TypeRef& lhs, const HIR::TypeRef& rhs) {
            // TODO: Only print when at the top-level
            TRACE_FUNCTION_F(lhs << " == " << rhs);
            // Match the types, letting lifetimes equate
            if(rhs.data().is_Diverge())
                return ;

            ASSERT_BUG(sp, lhs.data().tag() == rhs.data().tag(), "Mismatched types: " << lhs << " != " << rhs);
            TU_MATCH_HDRA( (lhs.data(), rhs.data()), { )
            TU_ARMA(Infer, l, r) BUG(sp, "");
            TU_ARMA(Primitive, l, r) {}
            TU_ARMA(Generic, l, r) {}
            TU_ARMA(Diverge, l, r) {}
            TU_ARMA(Path, l, r) {
                TU_MATCH_HDRA( (l.path.m_data, r.path.m_data), { )
                TU_ARMA(Generic, le, re) {
                    equate_pps(sp, le.m_params, re.m_params);
                    }
                TU_ARMA(UfcsKnown, le, re) {
                    equate_types(sp, le.type, re.type);
                    equate_pps(sp, le.params, re.params);
                    equate_pps(sp, le.trait.m_params, re.trait.m_params);
                    }
                TU_ARMA(UfcsInherent, le, re) {
                    equate_types(sp, le.type, re.type);
                    equate_pps(sp, le.params, re.params);
                    equate_pps(sp, le.impl_params, re.impl_params);
                    }
                TU_ARMA(UfcsUnknown, le, re) {
                    BUG(sp, lhs << " := " << rhs << ": UfcsUnknown");
                    }
                }
                }
            TU_ARMA(TraitObject, l, r) {
                // TODO: Sometimes the traits involved will make stricter lifetimes requirements (e.g. Any implies 'static)
                auto get_lifetime = [](const HIR::TypeData::Data_TraitObject& e)->HIR::LifetimeRef {
                    const auto& t = *e.m_trait.m_trait_ptr;
                    DEBUG(e.m_trait.m_path << " " << t.m_lifetime);
                    if( t.m_lifetime == HIR::LifetimeRef::new_static() ) {
                        return t.m_lifetime;
                    }
                    for(const auto& st : t.m_all_parent_traits) {
                    }
                    return e.m_lifetime;
                    };
                this->equate_lifetimes(sp, get_lifetime(l), get_lifetime(r));
                this->equate_traitpath(sp, l.m_trait, r.m_trait);
                ASSERT_BUG(sp, l.m_markers.size() == r.m_markers.size(), "");
                for(size_t i = 0; i < l.m_markers.size(); i ++)
                    this->equate_pps(sp, l.m_markers[i].m_params, r.m_markers[i].m_params);
                }
            TU_ARMA(ErasedType, l, r) {
                this->equate_lifetimes(sp, l.m_lifetime, r.m_lifetime);
                ASSERT_BUG(sp, l.m_traits.size() == r.m_traits.size(), "");
                for(size_t i = 0; i < l.m_traits.size(); i ++)
                    this->equate_traitpath(sp, l.m_traits[i], r.m_traits[i]);
                }
            TU_ARMA(Array, l, r) {
                this->equate_types(sp, l.inner, r.inner);
                }
            TU_ARMA(Slice, l, r) {
                this->equate_types(sp, l.inner, r.inner);
                }
            TU_ARMA(Tuple, l, r) {
                ASSERT_BUG(sp, l.size() == r.size(), "");
                for(size_t i = 0; i < l.size(); i ++)
                    this->equate_types(sp, l[i], r[i]);
                }
            TU_ARMA(Borrow, l, r) {
                this->equate_lifetimes(sp, l.lifetime, r.lifetime);
                this->equate_types(sp, l.inner, r.inner);
                }
            TU_ARMA(Pointer, l, r) {
                this->equate_types(sp, l.inner, r.inner);
                }
            TU_ARMA(Function, l, r) {
                // Handling required?
                }
            TU_ARMA(Closure, l, r) {
                ASSERT_BUG(sp, l.node == r.node, "");
                }
            TU_ARMA(Generator, l, r) {
                ASSERT_BUG(sp, l.node == r.node, "");
                }
            }
        }
        std::vector<HIR::LifetimeRef>   m_pattern_lifetime_stack;
        void equate_pattern_binding(const Span& sp, const HIR::PatternBinding& b, const HIR::TypeRef& src_ty, const HIR::ExprNode& root_node)
        {
            const HIR::TypeRef*   cur_ty = &src_ty;
            if( b.m_implicit_deref_count ) {
                TODO(sp, "Apply implicit derefs (binding) " << *cur_ty << " deref " << b.m_implicit_deref_count);
            }
            auto get_lft = [&]()->HIR::LifetimeRef {
                if(m_pattern_lifetime_stack.empty()) {
                    return get_borrow_lifetime(root_node, [&](const HIR::ExprNode& value){ return m_state.allocate_local(sp, root_node, value, b); });
                }
                else {
                    return m_pattern_lifetime_stack.back();
                }
            };
            const auto& slot = get_local_var_ty(sp, b.m_slot);
            switch(b.m_type)
            {
            case ::HIR::PatternBinding::Type::Move:
                this->equate_types(sp, slot, *cur_ty);
                break;
            case ::HIR::PatternBinding::Type::MutRef:
                this->equate_types(sp, slot, HIR::TypeRef::new_borrow(HIR::BorrowType::Unique, cur_ty->clone(), get_lft()));
                break;
            case ::HIR::PatternBinding::Type::Ref:
                this->equate_types(sp, slot, HIR::TypeRef::new_borrow(HIR::BorrowType::Shared, cur_ty->clone(), get_lft()));
                break;
            }
        }
        void equate_pattern(const Span& sp, const HIR::Pattern& pat, const HIR::TypeRef& src_ty, const HIR::ExprNode& root_node)
        {
            // Match pattern into the type (including bindings)
            if(src_ty.data().is_Diverge())
                return ;
            for(const auto& b : pat.m_bindings)
            {
                equate_pattern_binding(sp, b, src_ty, root_node);
            }
            const HIR::TypeRef*   cur_ty = &src_ty;
            size_t start_lifetime_stack_height = m_pattern_lifetime_stack.size();
            for(auto i = pat.m_implicit_deref_count; i --; )
            {
                if( const auto* tep = cur_ty->data().opt_Borrow() ) {
                    m_pattern_lifetime_stack.push_back(tep->lifetime);
                    cur_ty = &tep->inner;
                }
                else {
                    TODO(sp, "Apply implicit derefs - " << *cur_ty);
                }
            }
            TU_MATCH_HDRA( (pat.m_data),  { )
            TU_ARMA(Any, _) {}
            TU_ARMA(Slice, pe) {
                const auto& ity = cur_ty->data().is_Array() ? cur_ty->data().as_Array().inner : cur_ty->data().as_Slice().inner;
                for(const auto& subpat : pe.sub_patterns) {
                    equate_pattern(sp, subpat, ity, root_node); 
                }
                }
            TU_ARMA(SplitSlice, pe) {
                const auto& ity = cur_ty->data().is_Array() ? cur_ty->data().as_Array().inner : cur_ty->data().as_Slice().inner;
                for(const auto& subpat : pe.leading) {
                    equate_pattern(sp, subpat, ity, root_node); 
                }
                if(pe.extra_bind.is_valid()) {
                    equate_pattern_binding(sp, pe.extra_bind, *cur_ty, root_node);
                }
                for(const auto& subpat : pe.trailing) {
                    equate_pattern(sp, subpat, ity, root_node); 
                }
                }
            TU_ARMA(Tuple, pe) {
                const auto& te = cur_ty->data().as_Tuple();
                ASSERT_BUG(sp, pe.sub_patterns.size() == te.size(), "");
                for(size_t i = 0; i < te.size(); i ++)
                {
                    equate_pattern(sp, pe.sub_patterns[i], te[i], root_node);
                }
                }
            TU_ARMA(SplitTuple, pe) {
                const auto& te = cur_ty->data().as_Tuple();
                ASSERT_BUG(sp, pe.leading.size() + pe.trailing.size() <= te.size(), "");
                for(size_t i = 0; i < pe.leading.size(); i ++)
                {
                    equate_pattern(sp, pe.leading[i], te[i], root_node);
                }
                for(size_t i = 0; i < pe.trailing.size(); i ++)
                {
                    equate_pattern(sp, pe.trailing[i], te[te.size() - pe.trailing.size() + i], root_node);
                }
                }
            TU_ARMA(Box, pe) {
                TODO(sp, "Box patterns w/ " << *cur_ty);
                }
            TU_ARMA(Ref, pe) {
                const auto& te = cur_ty->data().as_Borrow();
                m_pattern_lifetime_stack.push_back(te.lifetime);
                equate_pattern(sp, *pe.sub, te.inner, root_node);
                m_pattern_lifetime_stack.pop_back();
                }

            TU_ARMA(Or, pe) {
                for(auto& subpat : pe) {
                    equate_pattern(sp, subpat, *cur_ty, root_node);
                }
                }
            
            TU_ARMA(PathTuple, pe) {
                const HIR::Struct::Data::Data_Tuple*    flds = nullptr;
                //HIR::TypeRef    ty;
                TU_MATCH_HDRA( (pe.binding), { )
                TU_ARMA(Unbound, pbe) {}
                TU_ARMA(Enum, pbe) {
                    const auto& sub_ty = pbe.ptr->m_data.as_Data().at(pbe.var_idx).type;
                    const auto& str = *sub_ty.data().as_Path().binding.as_Struct();
                    flds = &str.m_data.as_Tuple();
                    // Assume that the enum and the inner struct have the same parameter set
                    // TODO: Equate type?
                    }
                TU_ARMA(Struct, pbe) {
                    flds = &pbe->m_data.as_Tuple();
                    // TODO: Equate type?
                    }
                }
                assert(flds);
                auto ms = MonomorphStatePtr(nullptr, &pe.path.m_data.as_Generic().m_params, nullptr);
                ASSERT_BUG(sp, pe.leading.size() + pe.trailing.size() <= flds->size(), "");
                size_t trailing_start = flds->size() - pe.trailing.size();
                for(size_t i = 0; i < flds->size(); i++)
                {
                    if( i < pe.leading.size() ) {
                        auto exp_ty = m_resolve.monomorph_expand(sp, (*flds)[i].ent, ms);
                        equate_pattern(sp, pe.leading[i], exp_ty, root_node);
                    }
                    else if( i > trailing_start ) {
                        auto exp_ty = m_resolve.monomorph_expand(sp, (*flds)[i].ent, ms);
                        equate_pattern(sp, pe.trailing[i - trailing_start], exp_ty, root_node);
                    }
                    else {
                    }
                }
                }
            TU_ARMA(PathNamed, pe) {
                //this->equate_types(sp, 
                //TODO(sp, "PathNamed patterns w/ " << *cur_ty);
                }

            // No need to check lifetimes of values (they have zero impact)
            TU_ARMA(Value, pe) {
                }
            TU_ARMA(Range, pe) {
                }
            TU_ARMA(PathValue, pe) {
                // Might need to equate types?
                }
            }
            assert(m_pattern_lifetime_stack.size() >= start_lifetime_stack_height);
            m_pattern_lifetime_stack.resize(start_lifetime_stack_height);
        }

        struct Monomorph_AddLifetimes: public Monomorphiser {
            ExprVisitor_Enumerate& parent;

            ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& g) const override {
                return HIR::TypeRef(g.name, g.binding);
            }
            ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& g) const override {
                return g;
            }
            ::HIR::LifetimeRef get_lifetime(const Span& sp, const ::HIR::GenericRef& g) const override {
                if( g.group() == 3 ) {
                    TODO(sp, "Found HRL");
                }
                return HIR::LifetimeRef(g.binding);
            }

            ::HIR::LifetimeRef monomorph_lifetime(const Span& sp, const ::HIR::LifetimeRef& tpl) const override {
                if( tpl.binding == HIR::LifetimeRef::UNKNOWN || tpl.binding == HIR::LifetimeRef::INFER ) {
                    return parent.m_state.allocate_ivar(sp);
                }
                else {
                    ASSERT_BUG(sp, !parent.m_state.opt_ivar(sp, tpl), "Found ivar while adding lifetimes - " << tpl);
                    ASSERT_BUG(sp, !parent.m_state.opt_local(sp, tpl), "Found local while adding lifetimes - " << tpl);
                    return Monomorphiser::monomorph_lifetime(sp, tpl);
                }
            }

            Monomorph_AddLifetimes(ExprVisitor_Enumerate& parent): parent(parent) {}
        };
        Monomorph_AddLifetimes get_monomorph_add() {
            return Monomorph_AddLifetimes(*this);
        }
        void visit_path_params(HIR::PathParams& pps) override {
            TRACE_FUNCTION_FR(pps, pps);
            pps = get_monomorph_add().monomorph_path_params(Span(), pps, false);
        }
        void visit_type(HIR::TypeRef& ty) override {
            TRACE_FUNCTION_FR(ty, ty);
            ty = get_monomorph_add().monomorph_type(Span(), ty, false);
        }


        /// <summary>
        /// Obtain the root lifetime for a borrow operation
        /// </summary>
        /// <param name="node">Root node</param>
        /// <param name="cb">Function to get a local lifetime from the root value</param>
        /// <returns>Lifetime reference (a local, or the first dereferenced borrow)</returns>
        HIR::LifetimeRef get_borrow_lifetime(const ::HIR::ExprNode& node, std::function<HIR::LifetimeRef(const ::HIR::ExprNode&)> cb) const
        {
            // Determine a suitable lifetime for this value
            // - Deref? Grab lifetime of deref-ed value
            // - Static - 'static
            // - Local variable (really anything else) - allocate local
            struct V: public HIR::ExprVisitor {
                const ExprVisitor_Enumerate& m_parent;
                std::function<HIR::LifetimeRef(const ::HIR::ExprNode&)>& m_cb;
                HIR::LifetimeRef    m_res;

                V(const ExprVisitor_Enumerate& parent, std::function<HIR::LifetimeRef(const ::HIR::ExprNode&)>& cb)
                    : m_parent(parent)
                    , m_cb(cb)
                {
                }

                void local(const HIR::ExprNode& cur) {
                    m_res = m_cb(cur);
                }

                #define NV(nt)   void visit(HIR::nt& node) override { local(node); }
                NV(ExprNode_Block)
                NV(ExprNode_Asm)
                NV(ExprNode_Asm2)
                NV(ExprNode_Return)
                NV(ExprNode_Yield)
                NV(ExprNode_Let)
                NV(ExprNode_Loop)
                NV(ExprNode_LoopControl)
                NV(ExprNode_Match)
                NV(ExprNode_If)

                NV(ExprNode_Assign)
                NV(ExprNode_BinOp)
                NV(ExprNode_UniOp)
                NV(ExprNode_Borrow)
                NV(ExprNode_RawBorrow)
                NV(ExprNode_Cast)
                //NV(ExprNode_Unsize)

                NV(ExprNode_Emplace)

                NV(ExprNode_TupleVariant);
                NV(ExprNode_CallPath);
                NV(ExprNode_CallValue);
                NV(ExprNode_CallMethod);

                NV(ExprNode_Literal);
                NV(ExprNode_UnitVariant);
                NV(ExprNode_Variable);
                NV(ExprNode_ConstParam);

                NV(ExprNode_StructLiteral);
                NV(ExprNode_Tuple);
                NV(ExprNode_ArrayList);
                NV(ExprNode_ArraySized);

                NV(ExprNode_Closure);
                NV(ExprNode_Generator);
                NV(ExprNode_GeneratorWrapper);
                #undef NV

                void visit(HIR::ExprNode_Unsize& node) override {
                    // If the inner type is an array, then this will eventually become a `deref(unsize(borrow(...)))`
                    if( node.m_value->m_res_type.data().is_Array() ) {
                        node.m_value->visit(*this);
                    }
                    else {
                        local(node);
                    }
                }

                void visit(HIR::ExprNode_PathValue& node) override {
                    // If this is a static, return 'static
                    MonomorphState  ms;
                    auto v = m_parent.m_resolve.get_value(node.span(), node.m_path, ms, /*signature_only*/true);
                    if( v.is_Static() ) {
                        m_res = HIR::LifetimeRef::new_static();
                    }
                    else {
                        // Should this be possible? Maybe if there's generics going on.
                        local(node);
                    }
                }
                void visit(HIR::ExprNode_Deref& node) override {
                    TU_MATCH_HDRA( (node.m_value->m_res_type.data()), {)
                    default:
                        // For deref impls, just propagate through (the signature is `fn deref(&self) -> &Target`)
                        node.m_value->visit(*this);
                    TU_ARMA(Pointer, te) {
                        m_res = HIR::LifetimeRef::new_static();
                        }
                    TU_ARMA(Borrow, te) {
                        m_res = te.lifetime;
                        }
                    }
                }
                void visit(HIR::ExprNode_Index& node) override {
                    // Indexing (like field access) propagates inwards
                    node.m_value->visit(*this);
                }
                void visit(HIR::ExprNode_Field& node) override {
                    // Field access propagates inwards
                    node.m_value->visit(*this);
                }

            } v { *this, cb };
            const_cast<HIR::ExprNode&>(node).visit(v);
            ASSERT_BUG(node.span(), v.m_res != HIR::LifetimeRef(), "");
            return v.m_res;
        }

    public:
        void visit_node_ptr(::std::unique_ptr< ::HIR::ExprNode>& node_ptr) override {
            assert(node_ptr);
            visit_type(node_ptr->m_res_type);
            DEBUG("RES: " << node_ptr->m_res_type);
            node_ptr->visit(*this);
        }

        void visit(::HIR::ExprNode_Block& node) override {
            HIR::ExprVisitorDef::visit(node);
            if( node.m_value_node ) {
                equate_node_types(node, node.m_value_node);
            }
        }
        void visit(::HIR::ExprNode_Asm& node) override {
            HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_Asm2& node) override {
            HIR::ExprVisitorDef::visit(node);
        }
        void visit(::HIR::ExprNode_Return& node) override {
            HIR::ExprVisitorDef::visit(node);
            if( node.m_value ) {
                equate_types(node.m_value->span(), m_returns.empty() ? m_ret_ty : *m_returns.back(), node.m_value->m_res_type);
            }
        }
        void visit(::HIR::ExprNode_Yield& node) override {
            HIR::ExprVisitorDef::visit(node);
            TODO(node.span(), "Handle yield in lifetime inferrence");
        }
        void visit(::HIR::ExprNode_Let& node) override {
            HIR::ExprVisitorDef::visit(node);
            if( node.m_value ) {
                equate_pattern(node.span(), node.m_pattern, node.m_value->m_res_type, *node.m_value);
            }
        }
        void visit(::HIR::ExprNode_Loop& node) override {
            m_loops.push_back(&node);
            HIR::ExprVisitorDef::visit(node);
            m_loops.pop_back(/*&node*/);
        }
        void visit(::HIR::ExprNode_LoopControl& node) override {
            HIR::ExprVisitorDef::visit(node);
            if( node.m_value ) {
                auto it = ::std::find(this->m_loops.rbegin(), this->m_loops.rend(), node.m_target_node);
                ASSERT_BUG(node.span(), it != this->m_loops.rend(), "Loop target node not found in the loop stack");
                equate_types(node.m_value->span(), node.m_target_node->m_res_type, node.m_value->m_res_type);
            }
        }
        void visit(::HIR::ExprNode_Match& node) override {
            HIR::ExprVisitorDef::visit(node);
            for(auto& arm : node.m_arms) {
                for(auto& pat : arm.m_patterns)
                    equate_pattern(node.span(), pat, node.m_value->m_res_type, *node.m_value);
                equate_node_types(node, arm.m_code);
            }
        }
        void visit(::HIR::ExprNode_If& node) override {
            HIR::ExprVisitorDef::visit(node);
            // Equate both arms to the output
            if( node.m_false ) {
                equate_node_types(node, node.m_true );
                equate_node_types(node, node.m_false);
            }
        }

        void visit(::HIR::ExprNode_Assign& node) override {
            HIR::ExprVisitorDef::visit(node);
            if( node.m_op == HIR::ExprNode_Assign::Op::None ) {
                equate_types(node.span(), node.m_slot->m_res_type, node.m_value->m_res_type);
            }
        }
        void visit(::HIR::ExprNode_BinOp& node) override {
            HIR::ExprVisitorDef::visit(node);
            // No lifetimes involved.
        }
        void visit(::HIR::ExprNode_UniOp& node) override {
            HIR::ExprVisitorDef::visit(node);
            // No lifetimes involved.
        }


        void visit(::HIR::ExprNode_Borrow& node) override {
            HIR::ExprVisitorDef::visit(node);
            auto lft = get_borrow_lifetime(*node.m_value, [&](const HIR::ExprNode& value){ return m_state.allocate_local(node, value); });
            equate_types(node.span(), node.m_res_type, HIR::TypeRef::new_borrow(node.m_type, node.m_value->m_res_type.clone(), lft));
        }
        void visit(::HIR::ExprNode_RawBorrow& node) override {
            HIR::ExprVisitorDef::visit(node);
            // No lifetime!
        }
        void visit(::HIR::ExprNode_Cast& node) override {
            const auto& sp = node.span();
            HIR::ExprVisitorDef::visit(node);
            const auto& dst = node.m_dst_type;
            const auto& src = node.m_value->m_res_type;
            if( dst == src ) {
                this->equate_types(node.span(), dst, src);
            }
            else if( const auto* de = dst.data().opt_Function() ) {
                auto pp = de->hrls.make_empty_params(true);
                visit_path_params(pp);
                auto ms_d = MonomorphHrlsOnly(pp);
                if( const auto* se = src.data().opt_Function() ) {
                    //const auto& se = src.data().as_Function();
                    TODO(node.span(), "Propagate lifetimes through cast - " << dst << " := " << src);
                }
                else if( const auto* se = src.data().opt_Closure() ) {
                    const HIR::ExprNode_Closure& cnode = *se->node;
                    ASSERT_BUG(node.span(), de->m_arg_types.size() == cnode.m_args.size(), "");
                    for(size_t i = 0; i < de->m_arg_types.size(); i ++) {
                        this->equate_types(node.span(), ms_d.monomorph_type(sp, de->m_arg_types[i]), cnode.m_args[i].second);
                    }
                    this->equate_types(node.span(), ms_d.monomorph_type(sp, de->m_rettype), cnode.m_return);
                }
                else {
                    TODO(node.span(), "Propagate lifetimes through cast - " << dst << " := " << src);
                }
            }
            else if( dst.data().is_Primitive() || src.data().is_Primitive() ) {
                // Nothing to do
            }
            else if( dst.data().is_Pointer() || src.data().is_Pointer() ) {
                // TODO: What about trait objects? Should they maintain their internal liftimes?
            }
            else {
                TODO(node.span(), "Propagate lifetimes through cast - " << dst << " := " << src);
            }
            this->equate_types(node.span(), node.m_res_type, dst);
        }

        bool iterate_type_lifetime_bounds(const HIR::TypeRef& ty, std::function<bool(const HIR::LifetimeRef&)> cb) const
        {
            if( m_resolve.m_impl_generics ) {
                for(const auto& b : m_resolve.m_impl_generics->m_bounds) {
                    if( const auto* be = b.opt_TypeLifetime() ) {
                        if( be->type == ty )
                            if( cb(be->valid_for) )
                                return true;
                    }
                }
            }
            if( m_resolve.m_item_generics ) {
                for(const auto& b : m_resolve.m_item_generics->m_bounds) {
                    if( const auto* be = b.opt_TypeLifetime() )
                        if( be->type == ty )
                            if( cb(be->valid_for) )
                                return true;
                }
            }
            return false;
        }
        /// Extract lifetimes from a type and equate with the target lifetime.
        void equate_type_lifetimes(const Span& sp, const HIR::LifetimeRef& dst_lft, const HIR::TypeRef& ty)
        {
            visit_ty_with(ty, [&](const HIR::TypeRef& t)->bool {
                if(t.data().is_Borrow())
                    this->equate_lifetimes(sp, dst_lft, t.data().as_Borrow().lifetime);
                if(is_opaque(t)) {
                    // Iterate type lifetime bounds
                    iterate_type_lifetime_bounds(t, [&](const HIR::LifetimeRef& lft)->bool {
                        this->equate_lifetimes(sp, dst_lft, lft);
                        return false;
                        });
                    if( t.data().is_Generic() ) {
                        // If the above didn't return anything, then assign a "only this function" liftime
                    }
                    else {
                        TODO(sp, "Get lifetime (from bounds) for opaque type - " << t);
                    }
                }
                if( t.data().is_Path() && t.data().as_Path().path.m_data.is_Generic() ) {
                    for(const auto& l : t.data().as_Path().path.m_data.as_Generic().m_params.m_lifetimes)
                        this->equate_lifetimes(sp, dst_lft, l);
                }
                return false;
                });
        }
        static bool is_opaque(const ::HIR::TypeRef& ty) {
            if(ty.data().is_Generic())
                return true;
            if(ty.data().is_ErasedType())
                return true;
            if(ty.data().is_Path() && ty.data().as_Path().binding.is_Opaque())
                return true;
            return false;
        }
        void unsize_types(const Span& sp, const ::HIR::TypeRef& dst, const ::HIR::TypeRef& src) {
            if(dst == src) {
                this->equate_types(sp, dst, src);
            }
            else if( dst.data().is_Slice() && src.data().is_Array() ) {
                this->equate_types(sp,
                    dst.data().as_Slice().inner,
                    src.data().as_Array().inner
                    );
            }
            else if( dst.data().is_TraitObject() ) {
                const auto& dst_lft = dst.data().as_TraitObject().m_lifetime;
                equate_type_lifetimes(sp, dst_lft, src);
            }
            // If either side is an opaque type (generic/erased/opaque) then just look for an impl
            else if( is_opaque(dst) || is_opaque(src) ) {
                // TODO: Look for the `Unsize` impl? Could it provide a lifetime rule?
                TODO(sp, "Propagate lifetimes through unsize (generic) - " << dst << " := " << src);
            }
            // If the inner is a path, it must be a struct with an `Unsize` impl somewhere
            else if( dst.data().is_Path() ) {
                ASSERT_BUG(sp, src.data().is_Path(), "");
                ASSERT_BUG(sp, dst.data().as_Path().binding == src.data().as_Path().binding, "");
                ASSERT_BUG(sp, dst.data().as_Path().binding.is_Struct(), "");
                const ::HIR::Struct& str = *dst.data().as_Path().binding.as_Struct();
                const auto& dst_p = dst.data().as_Path().path.m_data.as_Generic();
                const auto& src_p = src.data().as_Path().path.m_data.as_Generic();
                auto param = str.m_struct_markings.unsized_param;

                return unsize_types(sp, dst_p.m_params.m_types.at(param), src_p.m_params.m_types.at(param));
            }
            // TODO: Look for an Unsize impl?
            else {
                TODO(sp, "Propagate lifetimes through unsize - " << dst << " := " << src);
            }
        }
        void coerce_unsize_types(const Span& sp, const ::HIR::TypeRef& dst, const ::HIR::TypeRef& src) {
            if(dst == src) {
                this->equate_types(sp, dst, src);
            }
            else if( auto* dst_te = dst.data().opt_Borrow() ) {
                auto& src_te = src.data().as_Borrow();
                // Must be from a borrow
                this->equate_lifetimes(sp, dst_te->lifetime, src_te.lifetime);

                unsize_types(sp, dst_te->inner, src_te.inner);
            }
            else if( dst.data().is_Slice() && src.data().is_Array() ) {
                this->equate_types(sp, dst.data().as_Slice().inner, src.data().as_Array().inner);
            }
            // If either side is an opaque type (generic/erased/opaque) then just look for an impl
            else if( is_opaque(dst) || is_opaque(src) ) {
                // TODO: Look for the `CoerceUnsize` impl? Could it provide a lifetime rule?
                TODO(sp, "Propagate lifetimes through unsize (generic) - " << dst << " := " << src);
            }
            // If the type is a path, it must be a struct with an `CoerceUnsize` impl somewhere
            else if( dst.data().is_Path() ) {
                ASSERT_BUG(sp, src.data().is_Path(), "");
                ASSERT_BUG(sp, dst.data().as_Path().binding == src.data().as_Path().binding, "");
                ASSERT_BUG(sp, dst.data().as_Path().binding.is_Struct(), "");
                const ::HIR::Struct& str = *dst.data().as_Path().binding.as_Struct();
                const auto& dst_p = dst.data().as_Path().path.m_data.as_Generic();
                const auto& src_p = src.data().as_Path().path.m_data.as_Generic();
                auto param = str.m_struct_markings.coerce_param;
                switch( str.m_struct_markings.coerce_unsized )
                {
                case HIR::StructMarkings::Coerce::None:
                    BUG(sp, "");
                case HIR::StructMarkings::Coerce::Passthrough:
                    return coerce_unsize_types(sp, dst_p.m_params.m_types.at(param), src_p.m_params.m_types.at(param));
                case HIR::StructMarkings::Coerce::Pointer:
                    return unsize_types(sp, dst_p.m_params.m_types.at(param), src_p.m_params.m_types.at(param));
                }
            }
            else {
                TODO(sp, "Propagate lifetimes through unsize - " << dst << " := " << src);
            }
        }
        void visit(::HIR::ExprNode_Unsize& node) override {
            HIR::ExprVisitorDef::visit(node);
            this->equate_types(node.span(), node.m_res_type, node.m_dst_type);

            this->coerce_unsize_types(node.span(), node.m_dst_type, node.m_value->m_res_type);
        }
        void visit(::HIR::ExprNode_Index& node) override {
            HIR::ExprVisitorDef::visit(node);
            auto ty = HIR::TypeRef::new_path(HIR::Path(
                node.m_value->m_res_type.clone(),
                HIR::GenericPath(m_resolve.m_crate.get_lang_item_path(node.span(), "index"), { node.m_index->m_res_type.clone() }),
                "Output"
                ), {});
            m_resolve.expand_associated_types(node.span(), ty);
            this->equate_types(node.span(), node.m_res_type, ty);
        }
        void visit(::HIR::ExprNode_Deref& node) override {
            HIR::ExprVisitorDef::visit(node);
            TU_MATCH_HDRA( (node.m_value->m_res_type.data()), {)
            default: {
                auto ty = HIR::TypeRef::new_path(HIR::Path(
                    node.m_value->m_res_type.clone(),
                    HIR::GenericPath(m_resolve.m_crate.get_lang_item_path(node.span(), "deref"), {}),
                    "Target"
                    ), {});
                m_resolve.expand_associated_types(node.span(), ty);
                this->equate_types(node.span(), node.m_res_type, ty);
                }
            TU_ARMA(Pointer, te) {
                this->equate_types(node.span(), node.m_res_type, te.inner);
                }
            TU_ARMA(Borrow, te) {
                this->equate_types(node.span(), node.m_res_type, te.inner);
                }
            }
        }
        void visit(::HIR::ExprNode_Field& node) override {
            HIR::ExprVisitorDef::visit(node);
            const auto& sp = node.span();
            const auto& str_ty = node.m_value->m_res_type;

            bool is_index = ( '0' <= node.m_field.c_str()[0] && node.m_field.c_str()[0] <= '9' );
            unsigned index = is_index ? std::strtol(node.m_field.c_str(), nullptr, 10) : ~0u;
            // TODO: De-duplicate this logic (shared by this AND the check pass)

            if( const auto* te = str_ty.data().opt_Tuple() )
            {
                ASSERT_BUG(sp, is_index, "Non-index _Field on tuple");
                this->equate_types(sp, node.m_res_type, te->at(index));
            }
            else if( str_ty.data().is_Closure() )
            {
                BUG(sp, "Closure type being accessed too early");
            }
            else
            {
                ASSERT_BUG(sp, str_ty.data().is_Path(), "Value type of _Field isn't Path - " << str_ty);
                const auto& ty_e = str_ty.data().as_Path();
                const HIR::TypeRef* fld_ty_ptr = nullptr;
                if( const auto* strpp = ty_e.binding.opt_Struct() )
                {
                    const HIR::Struct& str = **strpp;
                    ASSERT_BUG(sp, is_index == str.m_data.is_Tuple(), "");
                    if( is_index ) {
                        const auto& flds = str.m_data.as_Tuple();
                        ASSERT_BUG(sp, index < flds.size(), "");
                        fld_ty_ptr = &flds[index].ent;
                    }
                    else {
                        const auto& flds = str.m_data.as_Named();
                        auto it = std::find_if(flds.begin(), flds.end(), [&](const auto& f) { return f.first == node.m_field; });
                        ASSERT_BUG(sp, it != flds.end(), "");
                        fld_ty_ptr = &it->second.ent;
                    }
                }
                else if( const auto* unnpp = ty_e.binding.opt_Union() )
                {
                    const HIR::Union& unn = **unnpp;
                    const auto& flds = unn.m_variants;
                    auto it = std::find_if(flds.begin(), flds.end(), [&](const auto& f) { return f.first == node.m_field; });
                    ASSERT_BUG(sp, it != flds.end(), "");
                    fld_ty_ptr = &it->second.ent;
                }
                else
                {
                    BUG(sp, "Value type of _Field isn't a Struct or Union - " << str_ty);
                }
                assert(fld_ty_ptr);

                auto ms = MonomorphStatePtr(&str_ty, &ty_e.path.m_data.as_Generic().m_params, nullptr);
                auto fld_ty = m_resolve.monomorph_expand(sp, *fld_ty_ptr, ms);

                this->equate_types(sp, node.m_res_type, fld_ty);
            }
        }

        void visit(::HIR::ExprNode_CallPath& node) override {
            HIR::ExprVisitorDef::visit(node);

            // Equate arguments and returns (monomorphised)
            MonomorphState  ms;
            auto v = m_resolve.get_value(node.span(), node.m_path, ms, /*signature_only*/true);
            if( const auto* pe = node.m_path.m_data.opt_UfcsInherent() ) {
                this->equate_pps(node.span(), pe->impl_params, *ms.pp_impl);
            }
            const auto& fcn = *v.as_Function();

            for(size_t i = 0; i < fcn.m_args.size(); i ++)
            {
                auto arg_ty = m_resolve.monomorph_expand(node.m_args[i]->span(), fcn.m_args[i].second, ms);
                this->equate_types(node.m_args[i]->span(), arg_ty, node.m_args[i]->m_res_type);
                this->equate_types(node.m_args[i]->span(), node.m_cache.m_arg_types[i], arg_ty);
            }
            this->equate_types(node.span(), node.m_cache.m_arg_types.back(), m_resolve.monomorph_expand(node.span(), fcn.m_return, ms));
            this->equate_types(node.span(), node.m_res_type, node.m_cache.m_arg_types.back());
        }
        void visit(::HIR::ExprNode_CallValue& node) override {
            HIR::ExprVisitorDef::visit(node);
            TRACE_FUNCTION_FR("_CallValue: m_value->m_res_type=" << node.m_value->m_res_type, "_CallValue");
            // TODO: Equate arguments and returns (after monomorphising away the HRLs)
            const auto& val_ty = node.m_value->m_res_type;
            if( const auto* tep = val_ty.data().opt_Function() ) {
                ::HIR::PathParams   hrl_params = tep->hrls.make_empty_params(true);
                this->visit_path_params(hrl_params);
                auto ms = MonomorphHrlsOnly(hrl_params);

                ASSERT_BUG(node.span(), tep->m_arg_types.size() == node.m_args.size(), "");
                for(size_t i = 0; i < node.m_args.size(); i ++) {
                    this->equate_types(node.m_args[i]->span(), ms.monomorph_type(node.span(), tep->m_arg_types[i], false), node.m_args[i]->m_res_type);
                }
                this->equate_types(node.span(), node.m_res_type, ms.monomorph_type(node.span(), tep->m_rettype, false));
            }
            else if( const auto* tep = val_ty.data().opt_Closure() ) {
                if( tep->node->m_obj_path.m_path != HIR::SimplePath() ) {
                    TODO(node.span(), "Handle CallValue (expanded) - " << val_ty);
                }
                else {
                    ASSERT_BUG(node.span(), tep->node->m_args.size() == node.m_args.size(), "");
                    for(size_t i = 0; i < node.m_args.size(); i ++) {
                        this->equate_types(node.m_args[i]->span(), tep->node->m_args[i].second, node.m_args[i]->m_res_type);
                    }
                    this->equate_types(node.span(), node.m_res_type, tep->node->m_return);
                }
            }
            else if( val_ty.data().is_Path() || val_ty.data().is_Generic() || val_ty.data().is_ErasedType() ) {
                // Look up the trait impl and check the generics on it
                // - If it's a bound, then handle the HRLs
                // - If an impl ref, just as normal.
                auto trait = m_resolve.m_crate.get_lang_item_path(node.span(), "fn_once");
                ::std::vector< ::HIR::TypeRef>  tup_ents;
                for(const auto& arg : node.m_args) {
                    tup_ents.push_back( arg->m_res_type.clone() );
                }
                ::HIR::PathParams   params;
                params.m_types.push_back( ::HIR::TypeRef( mv$(tup_ents) ) );

                bool found = m_resolve.find_impl(node.span(), trait, &params, val_ty, [&](ImplRef impl_ref, bool fuzzy)->bool{
                    ASSERT_BUG(node.span(), !fuzzy, "Fuzzy match in check pass");

                    TU_MATCH_HDRA( (impl_ref.m_data), { )
                    TU_ARMA(TraitImpl, e) {
                        TODO(node.span(), "Handle CallValue (trait impl) - " << val_ty);
                        }
                    TU_ARMA(Bounded, e) {
                        if( e.hrls.m_lifetimes.size() > 0 ) {
                            // Monomorphise with some new lifetime params
                            ::HIR::PathParams   hrl_params = e.hrls.make_empty_params(true);
                            this->visit_path_params(hrl_params);
                            auto ms = MonomorphHrlsOnly(hrl_params);
                            auto mm_trait_args = ms.monomorph_path_params(node.span(), e.trait_args, true);
                            auto mm_res = ms.monomorph_type(node.span(), e.assoc.at("Output").type, true);
                            DEBUG("mm_trait_args=" << mm_trait_args << " mm_res=" << mm_res);

                            equate_pps(node.span(), mm_trait_args, params); 
                            equate_types(node.span(), node.m_res_type, mm_res);
                        }
                        else {
                            equate_pps(node.span(), e.trait_args, params); 
                            equate_types(node.span(), node.m_res_type, impl_ref.get_type("Output"));
                        }
                        }
                    TU_ARMA(BoundedPtr, e) {
                        if( e.hrls && e.hrls->m_lifetimes.size() > 0 ) {
                            // Monomorphise with some new lifetime params
                            ::HIR::PathParams   hrl_params = e.hrls->make_empty_params(true);
                            this->visit_path_params(hrl_params);
                            auto ms = MonomorphHrlsOnly(hrl_params);
                            auto mm_trait_args = ms.monomorph_path_params(node.span(), *e.trait_args, true);
                            auto mm_res = ms.monomorph_type(node.span(), e.assoc->at("Output").type, true);
                            DEBUG("mm_trait_args=" << mm_trait_args << " mm_res=" << mm_res);

                            equate_pps(node.span(), mm_trait_args, params); 
                            equate_types(node.span(), node.m_res_type, mm_res);
                        }
                        else {
                            equate_pps(node.span(), *e.trait_args, params); 
                            equate_types(node.span(), node.m_res_type, impl_ref.get_type("Output"));
                        }
                        }
                    }

                    return true;
                    });
                if( !found ) {
                    ERROR(node.span(), E0000, "Unable to find a matching impl of " << trait << " for " << val_ty);
                }
            }
            else {
                TODO(node.span(), "Handle CallValue - " << val_ty);
            }
        }
        void visit(::HIR::ExprNode_CallMethod& node) override {
            HIR::ExprVisitorDef::visit(node);

            TRACE_FUNCTION_FR("_CallMethod", "_CallMethod");
            // Equate arguments and returns (monomorphised)
            MonomorphState  ms;
            auto v = m_resolve.get_value(node.span(), node.m_method_path, ms, /*signature_only*/true);
            if( const auto* pe = node.m_method_path.m_data.opt_UfcsInherent() ) {
                this->equate_pps(node.span(), pe->impl_params, *ms.pp_impl);
            }
            const auto& fcn = *v.as_Function();

            for(size_t i = 0; i < fcn.m_args.size(); i ++)
            {
                const auto& n = (i == 0 ? node.m_value : node.m_args[i-1]);
                auto arg_ty = m_resolve.monomorph_expand(n->span(), fcn.m_args[i].second, ms);
                DEBUG("ARG " << arg_ty);
                this->equate_types(n->span(), arg_ty, n->m_res_type);
                this->equate_types(n->span(), node.m_cache.m_arg_types[i], arg_ty);
            }
            DEBUG("RET " << fcn.m_return);
            this->equate_types(node.span(), node.m_cache.m_arg_types.back(), m_resolve.monomorph_expand(node.span(), fcn.m_return, ms));
            this->equate_types(node.span(), node.m_res_type, node.m_cache.m_arg_types.back());
        }
        
        void visit(::HIR::ExprNode_Literal& node) override {
            if( node.m_data.is_String() || node.m_data.is_ByteString() ) {
                const auto& be = node.m_res_type.data().as_Borrow();
                this->equate_lifetimes(node.span(), be.lifetime, ::HIR::LifetimeRef::new_static());
            }
        }
        void visit(::HIR::ExprNode_UnitVariant& node) override {
            HIR::ExprVisitorDef::visit(node);
            
            // Just assign the type through (for the path params)
            this->equate_pps(node.span(),
                node.m_res_type.data().as_Path().path.m_data.as_Generic().m_params,
                node.m_path.m_params
                );
        }
        void visit(::HIR::ExprNode_PathValue& node) override {
            const Span& sp = node.span();
            HIR::ExprVisitorDef::visit(node);
            MonomorphState  ms;
            auto v = m_resolve.get_value(sp, node.m_path, ms, /*signature_only*/true);
            if( const auto* pe = node.m_path.m_data.opt_UfcsInherent() ) {
                ms.pp_impl = &pe->impl_params;
            }

            HIR::TypeRef    ty;
            TU_MATCH_HDRA( (v), { )
            default:
                TODO(node.span(), "PathValue - " << node.m_path << " - " << v.tag_str());
            TU_ARMA(EnumConstructor, ve) {
                const auto& variant_ty = ve.e->m_data.as_Data().at(ve.v).type;
                const auto& variant_path = variant_ty.data().as_Path().path.m_data.as_Generic();
                const auto& str = *variant_ty.data().as_Path().binding.as_Struct();
                const auto& fields = str.m_data.as_Tuple();

                auto p = node.m_path.m_data.as_Generic().m_path;
                p.m_components.pop_back();

                ::HIR::FunctionType ft {
                    HIR::GenericParams(),   // TODO: Get HRLs?
                    false, ABI_RUST,
                    HIR::TypeRef::new_path(HIR::GenericPath(p, node.m_path.m_data.as_Generic().m_params.clone()), ve.e),
                    {}
                    };

                auto ms = MonomorphStatePtr(nullptr, &node.m_path.m_data.as_Generic().m_params, nullptr);
                for(const auto& var : fields) {
                    ft.m_arg_types.push_back( m_resolve.monomorph_expand(sp, var.ent, ms) );
                }
                ty = ::HIR::TypeRef(mv$(ft));
                }
            TU_ARMA(StructConstructor, ve) {
                const auto& str = *ve.s;
                const auto& fields = str.m_data.as_Tuple();

                ::HIR::FunctionType ft {
                    HIR::GenericParams(),   // TODO: Get HRLs?
                    false, ABI_RUST,
                    HIR::TypeRef::new_path(node.m_path.m_data.as_Generic().clone(), ve.s),
                    {}
                };

                auto ms = MonomorphStatePtr(nullptr, &node.m_path.m_data.as_Generic().m_params, nullptr);
                for(const auto& var : fields) {
                    ft.m_arg_types.push_back( m_resolve.monomorph_expand(sp, var.ent, ms) );
                }
                ty = ::HIR::TypeRef(mv$(ft));
                }
            TU_ARMA(Constant, ve) {
                ty = m_resolve.monomorph_expand(sp, ve->m_type, ms);
                }
            TU_ARMA(Static, ve) {
                ty = m_resolve.monomorph_expand(sp, ve->m_type, ms);
                }
            TU_ARMA(Function, ve) {
                ::HIR::FunctionType ft {
                    HIR::GenericParams(),
                    ve->m_unsafe, ve->m_abi,
                    m_resolve.monomorph_expand(sp, ve->m_return, ms),
                    {}
                };
                ft.hrls.m_lifetimes = ve->m_params.m_lifetimes;
                auto method_pp_trimmed = ms.pp_method->clone();
                method_pp_trimmed.m_lifetimes = std::move(ft.hrls.make_nop_params(3).m_lifetimes);
                ms.pp_method = &method_pp_trimmed;
                for(const auto& arg : ve->m_args)
                    ft.m_arg_types.push_back( m_resolve.monomorph_expand(sp, arg.second, ms) );
                ty = ::HIR::TypeRef(mv$(ft));
                //ty = m_resolve.monomorph_expand(node.span(), ve->make_pointer_type(), ms);
                }
            }
            this->equate_types(sp, node.m_res_type, ty);
        }
        void visit(::HIR::ExprNode_Variable& node) override {
            HIR::ExprVisitorDef::visit(node);
            this->equate_types(node.span(), node.m_res_type, get_local_var_ty(node.span(), node.m_slot));
        }
        void visit(::HIR::ExprNode_ConstParam& node) override {
            HIR::ExprVisitorDef::visit(node);
            // Nothing to do?
        }
        
        void visit(::HIR::ExprNode_Emplace& node) override {
            HIR::ExprVisitorDef::visit(node);
            TODO(node.span(), "Emplace");
        }
        void visit(::HIR::ExprNode_TupleVariant& node) override {
            const Span& sp = node.span();
            HIR::ExprVisitorDef::visit(node);
            
            // Just assign the type through (for the path params)
            this->equate_pps(node.span(),
                node.m_res_type.data().as_Path().path.m_data.as_Generic().m_params,
                node.m_path.m_params
                );

            const auto& ty = node.m_res_type;

            const ::HIR::t_tuple_fields* fields_ptr = nullptr;
            ASSERT_BUG(sp, ty.data().is_Path(), "Result type of _TupleVariant isn't Path");
            TU_MATCH_HDRA( (ty.data().as_Path().binding), {)
            TU_ARMA(Unbound, e) {
                BUG(sp, "Unbound type in _TupleVariant - " << ty);
                }
            TU_ARMA(Opaque, e) {
                BUG(sp, "Opaque type binding in _TupleVariant - " << ty);
                }
            TU_ARMA(Enum, e) {
                const auto& var_name = node.m_path.m_path.m_components.back();
                const auto& enm = *e;
                size_t idx = enm.find_variant(var_name);
                const auto& var_ty = enm.m_data.as_Data()[idx].type;
                const auto& str = *var_ty.data().as_Path().binding.as_Struct();
                ASSERT_BUG(sp, str.m_data.is_Tuple(), "Pointed variant of TupleVariant (" << node.m_path << ") isn't a Tuple");
                fields_ptr = &str.m_data.as_Tuple();
                }
            TU_ARMA(Union, e) {
                BUG(sp, "Union in TupleVariant");
                }
            TU_ARMA(ExternType, e) {
                BUG(sp, "ExternType in TupleVariant");
                }
            TU_ARMA(Struct, e) {
                ASSERT_BUG(sp, e->m_data.is_Tuple(), "Pointed struct in TupleVariant (" << node.m_path << ") isn't a Tuple");
                fields_ptr = &e->m_data.as_Tuple();
                }
            }
            assert(fields_ptr);
            const ::HIR::t_tuple_fields& fields = *fields_ptr;
            ASSERT_BUG(sp, fields.size() == node.m_args.size(), "");
            auto ms = MonomorphStatePtr(&ty, &ty.data().as_Path().path.m_data.as_Generic().m_params, nullptr);

            // Bind fields with type params (coercable)
            for( unsigned int i = 0; i < node.m_args.size(); i ++ )
            {
                const auto& des_ty_r = fields[i].ent;
                auto des_ty = m_resolve.monomorph_expand(node.m_args[i]->span(), des_ty_r, ms);
                this->equate_types(node.m_args[i]->span(), des_ty, node.m_args[i]->m_res_type);
            }
        }
        void visit(::HIR::ExprNode_StructLiteral& node) override {
            HIR::ExprVisitorDef::visit(node);
            if( node.m_base_value ) {
                this->equate_types( node.m_base_value->span(), node.m_res_type, node.m_base_value->m_res_type );
            }
            
            
            // Just assign the type through (for the path params)
            this->equate_pps(node.span(),
                node.m_res_type.data().as_Path().path.m_data.as_Generic().m_params,
                node.m_real_path.m_params
                );
            
            const Span& sp = node.span();
            const auto& ty_path = node.m_real_path;
            const auto& ty = node.m_res_type;
            ASSERT_BUG(sp, ty.data().is_Path(), "Result type of _StructLiteral isn't Path");

            const ::HIR::t_struct_fields* fields_ptr = nullptr;
            TU_MATCH_HDRA( (ty.data().as_Path().binding), {)
            TU_ARMA(Unbound, e) {}
            TU_ARMA(Opaque, e) {}
            TU_ARMA(Enum, e) {
                const auto& var_name = ty_path.m_path.m_components.back();
                const auto& enm = *e;
                auto idx = enm.find_variant(var_name);
                ASSERT_BUG(sp, idx != SIZE_MAX, "");
                ASSERT_BUG(sp, enm.m_data.is_Data(), "");
                const auto& var = enm.m_data.as_Data()[idx];

                const auto& str = *var.type.data().as_Path().binding.as_Struct();
                ASSERT_BUG(sp, var.is_struct, "Struct literal for enum on non-struct variant");
                fields_ptr = &str.m_data.as_Named();
                }
            TU_ARMA(Union, e) {
                fields_ptr = &e->m_variants;
                ASSERT_BUG(sp, node.m_values.size() > 0, "Union with no values");
                ASSERT_BUG(sp, node.m_values.size() == 1, "Union with multiple values");
                ASSERT_BUG(sp, !node.m_base_value, "Union can't have a base value");
                }
            TU_ARMA(ExternType, e) {
                BUG(sp, "ExternType in StructLiteral");
                }
            TU_ARMA(Struct, e) {
                if( e->m_data.is_Unit() )
                {
                    ASSERT_BUG(sp, node.m_values.size() == 0, "Values provided for unit-like struct");
                    ASSERT_BUG(sp, ! node.m_base_value, "Values provided for unit-like struct");
                    return ;
                }

                ASSERT_BUG(sp, e->m_data.is_Named(), "StructLiteral not pointing to a braced struct, instead " << e->m_data.tag_str() << " - " << ty);
                fields_ptr = &e->m_data.as_Named();
                }
            }
            ASSERT_BUG(node.span(), fields_ptr, "Didn't get field for path in _StructLiteral - " << ty);
            const ::HIR::t_struct_fields& fields = *fields_ptr;
            for(const auto& fld : fields) {
                DEBUG(fld.first << ": " << fld.second.ent);
            }
            auto ms = MonomorphStatePtr(&ty, &ty_path.m_params, nullptr);


            // Bind fields with type params (coercable)
            for(auto& val : node.m_values)
            {
                const auto& name = val.first;
                auto it = ::std::find_if(fields.begin(), fields.end(), [&](const auto& v)->bool{ return v.first == name; });
                assert(it != fields.end());
                const auto& des_ty_r = it->second.ent;

                DEBUG(name << " : " << des_ty_r);
                auto des_ty = m_resolve.monomorph_expand(val.second->span(), des_ty_r, ms);
                DEBUG("." << name << " : " << des_ty);
                this->equate_types(val.second->span(), des_ty, val.second->m_res_type);
            }
        }
        void visit(::HIR::ExprNode_Tuple& node) override {
            HIR::ExprVisitorDef::visit(node);
            // Destrucure output type
            auto& te = node.m_res_type.data().as_Tuple();
            for(size_t i = 0; i < te.size(); i ++)
            {
                auto& vnode = *node.m_vals[i];
                this->equate_types(vnode.span(), te[i], vnode.m_res_type);
            }
        }
        void visit(::HIR::ExprNode_ArrayList& node) override {
            HIR::ExprVisitorDef::visit(node);
            const auto& inner = node.m_res_type.data().as_Array().inner;
            for(auto& v : node.m_vals) {
                this->equate_types(v->span(), inner, v->m_res_type);
            }
        }
        void visit(::HIR::ExprNode_ArraySized& node) override {
            HIR::ExprVisitorDef::visit(node);
            const auto& inner = node.m_res_type.data().as_Array().inner;
            this->equate_types(node.m_val->span(), inner, node.m_val->m_res_type);
        }

        void visit(::HIR::ExprNode_Closure& node) override {
            m_returns.push_back(&node.m_return);
            HIR::ExprVisitorDef::visit(node);
            m_returns.pop_back();
        }
        void visit(::HIR::ExprNode_Generator& node) override {
            m_returns.push_back(&node.m_return);
            // TODO: Yield?
            HIR::ExprVisitorDef::visit(node);
            m_returns.pop_back();
        }
        void visit(::HIR::ExprNode_GeneratorWrapper& node) override {
            BUG(node.span(), "Encountered ExprNode_GeneratorWrapper too early");
        }
    };

    /// <summary>
    /// Visitor to compact (de-duplicate) types
    /// </summary>
    struct ExprVisitor_CompactTypes: HIR::ExprVisitorDef
    {
        std::map<std::string, HIR::TypeRef>  types;

        void visit_root(HIR::ExprPtr& ep)
        {
            ep->visit(*this);
            for(auto& b : ep.m_bindings) {
                visit_type(b);
            }
            for(auto& b : ep.m_erased_types) {
                visit_type(b);
            }
        }
        void visit_type(HIR::TypeRef& ty) override
        {
            HIR::ExprVisitorDef::visit_type(ty);

            // Use string comparison to ensure that lifetimes are checked
            auto s = FMT(ty);
            if(s[0] == '{') {
                auto p = s.find('}');
                s = s.substr(p+1);
            }

            auto it = types.find(s);
            if( it != types.end() ) {
                ty = HIR::TypeRef(it->second);
            }
            else {
                types.insert(std::make_pair(s, ty));
            }
        };
    };


    void HIR_Expand_LifetimeInfer_ExprInner(const StaticTraitResolve& resolve, const ::HIR::Function::args_t& args, const HIR::TypeRef& ret_ty, HIR::ExprPtr& ep)
    {
        LifetimeInferState  state { resolve };

        // Before running algorithm, dump the HIR (just as a reference for debugging)
        DEBUG("\n" << FMT_CB(os, HIR_DumpExpr(os, ep)));

        // Enumerate lifetimes and relationships
        {
            // TODO: Also do lifetime equality of the return type and the ATY version
            ExprVisitor_Enumerate   ev(resolve, args, ret_ty, state);
            ev.visit_root(ep);
        }

        // Shortcut for when there's nothing to infer
        if( state.m_ivars.size() == 0 ) {
            ExprVisitor_CompactTypes().visit_root(ep);
            return ;
        }

        // Before running algorithm, dump the HIR (just as a reference for debugging)
        DEBUG("\n" << FMT_CB(os, HIR_DumpExpr(os, ep)));

        // If there were inferred liftimes present

        for(size_t cur_iter = 0, remaining_iters = 1000; remaining_iters--; cur_iter ++ )
        {
            TRACE_FUNCTION_FR("=== Iter " << cur_iter << " ===", "=== Iter " << cur_iter << " ===");
            state.dump();

            bool change = false;
            auto set = [&](LifetimeInferState::IvarLifetime& iv, const HIR::LifetimeRef& lft, const char* log_reason) {
                assert(!iv.is_known());
                auto real_lft = state.get_final_lft(iv.sp, lft);
                DEBUG(state.get_lft_for_iv(iv) << " := " << real_lft << "[" << lft << "] (" << log_reason << ")");
                ASSERT_BUG(iv.sp, real_lft != state.get_lft_for_iv(iv), real_lft);
                iv.known = real_lft;
                change = true;
                };


            // Run through the ivar list, looking for ones with only one source/destination
            for(auto& iv : state.m_ivars) {
                if( !iv.is_known() ) {
                    // Zero sources - set to `'static`
                    if( iv.sources.size() == 0 ) {
                        set(iv, HIR::LifetimeRef::new_static(), "No source");
                        continue ;
                    }

                    if( iv.sources.size() == 1 ) {
                        auto lft = state.get_final_lft(iv.sp, iv.sources[0]);
                        //auto lft = iv.sources[0];
                        if( !state.opt_ivar(iv.sp, lft) ) {
                            set(iv, lft, "Only source");
                            continue ;
                        }
                    }
                    if( iv.destinations.size() == 1 ) {
                        // Don't let ivars propagate upwards
                        auto lft = state.get_final_lft(iv.sp, iv.destinations[0]);
                        if( !state.opt_ivar(iv.sp, lft) ) {
                            set(iv, lft, "Only destination");
                            continue ;
                        }
                    }

                    // If all sources are known, then unify
                    if( std::all_of(iv.sources.begin(), iv.sources.end(), [&](const HIR::LifetimeRef& lft) { return state.opt_ivar(iv.sp, lft) == nullptr; }) )
                    {
                        // De-duplicate
                        std::vector<HIR::LifetimeRef>   dedup_sources;
                        for(const auto& s : iv.sources) {
                            auto lft = state.get_final_lft(iv.sp, s);
                            if( std::find(dedup_sources.begin(), dedup_sources.end(), lft) == dedup_sources.end() )
                                dedup_sources.push_back(lft);
                        }

                        if( dedup_sources.size() == 1 ) {
                            set(iv, dedup_sources[0], "Only source (after dedup)");
                        }
                        else {
                            // Create a composite local lifetime
                            auto lft = state.allocate_local(iv.sp, std::move(dedup_sources));
                            set(iv, lft, "Composite");
                        }
                        continue ;
                    }
                }
            }

            // If all ivars are known, break
            if( std::all_of(state.m_ivars.begin(), state.m_ivars.end(), [](const LifetimeInferState::IvarLifetime& iv){ return iv.is_known(); }) ) {
                break;
            }
            // If there was no change, bail
            if( !change || remaining_iters == 0 ) {
                state.dump();
                BUG(ep->span(), "Lifetime inferrence stalled");
            }
            // Compact the lifetime list (resolve all known lifetimes to their actual value)
            for(auto& iv : state.m_ivars) {
                for(auto& l : iv.sources)
                    l = state.get_final_lft(iv.sp, l);
                for(auto& l : iv.destinations)
                    l = state.get_final_lft(iv.sp, l);
            }
        }

        {
            TRACE_FUNCTION_FR("COMPACT", "COMPACT");
            state.dump();

            // Compact (replace any mentioned ivars by the their known lifetime)
            for(auto& iv : state.m_ivars)
            {
                ASSERT_BUG(iv.sp, iv.is_known(), "Unresolved lifetime?");
                iv.known = state.get_final_lft(iv.sp, iv.known);
                ASSERT_BUG(iv.sp, !state.opt_ivar(iv.sp, iv.known), "Lifetime resolved to an ivar - " << state.get_lft_for_iv(iv) << " = " << iv.known);
                for(auto& l : iv.destinations)
                    l = state.get_final_lft(iv.sp, l);
            }
        }

        // Validate (check that everything roughly satisfies lifetime requirements)
        {
            TRACE_FUNCTION_FR("VALIDATE", "VALIDATE");
            for(auto& iv : state.m_ivars)
            {
                DEBUG("--" << state.get_lft_for_iv(iv));
                for(const auto& d : iv.destinations)
                    state.ensure_outlives(iv.sp, d, iv.known);
            }
        }


        // ---
        // Visit lifetimes in tree, updating all of them
        // ---
        struct Monomorph_CommitLifetimes: public Monomorphiser
        {
            LifetimeInferState& state;

            Monomorph_CommitLifetimes(LifetimeInferState& state): state(state) {}

            ::HIR::TypeRef get_type(const Span& sp, const ::HIR::GenericRef& g) const override {
                return HIR::TypeRef(g.name, g.binding);
            }
            ::HIR::ConstGeneric get_value(const Span& sp, const ::HIR::GenericRef& g) const override {
                return g;
            }
            ::HIR::LifetimeRef get_lifetime(const Span& sp, const ::HIR::GenericRef& g) const override {
                return HIR::LifetimeRef(g.binding);
            }

            ::HIR::LifetimeRef monomorph_lifetime(const Span& sp, const ::HIR::LifetimeRef& tpl) const override {
                if( tpl.binding == HIR::LifetimeRef::UNKNOWN || tpl.binding == HIR::LifetimeRef::INFER ) {
                    BUG(sp, "Unexpected unknown lifetime");
                }
                else if( const auto* iv = state.opt_ivar(sp, tpl) ) {
                    return iv->known;
                }
                else {
                    return tpl;
                }
            }
        } ms(state);
        {
            struct Visitor_CommitLifetimes: HIR::ExprVisitorDef
            {
                Monomorph_CommitLifetimes&   ms;
                Visitor_CommitLifetimes(Monomorph_CommitLifetimes& ms): ms(ms) {}

                void visit_root(HIR::ExprPtr& ep) {
                    for(auto& ty : ep.m_bindings) {
                        this->visit_type(ty);
                    }

                    this->visit_type(ep->m_res_type);
                    ep->visit(*this);
                }

                void visit_node_ptr(::std::unique_ptr< ::HIR::ExprNode>& node_ptr) override {
                    assert(node_ptr);
                    visit_type(node_ptr->m_res_type);
                    HIR::ExprVisitorDef::visit_node_ptr(node_ptr);
                }
                void visit_type(HIR::TypeRef& ty) override {
                    TRACE_FUNCTION_FR(ty, ty);
                    ty = ms.monomorph_type(Span(), ty, false);
                }
                void visit_path_params(HIR::PathParams& pp) override {
                    pp = ms.monomorph_path_params(Span(), pp, false);
                }
                //void visit(HIR::ExprNode_CallMethod& node) override {
                //    node.m_params = ms.monomorph_path_params(node.span(), node.m_params, false);
                //}
            } visitor(ms);
            visitor.visit_root(ep);
        }

        // Run type de-duplication over the tree again
        ExprVisitor_CompactTypes().visit_root(ep);
        DEBUG("\n" << FMT_CB(os, HIR_DumpExpr(os, ep)));
    }

    class OuterVisitor:
        public ::HIR::Visitor
    {
        StaticTraitResolve  m_resolve;
    public:
        OuterVisitor(const ::HIR::Crate& crate):
            m_resolve(crate)
        {}

        void check(const HIR::TypeRef& ret_ty, const ::HIR::Function::args_t& args, HIR::ExprPtr& root)
        {
            if( root.m_mir ) {
                DEBUG("MIR present, skipping");
                return ;
            }
            HIR_Expand_LifetimeInfer_ExprInner(m_resolve, args, ret_ty, root);
        }

        // NOTE: This is left here to ensure that any expressions that aren't handled by higher code cause a failure
        void visit_expr(::HIR::ExprPtr& exp) override {
            BUG(Span(), "visit_expr hit in OuterVisitor");
        }

        void visit_type(::HIR::TypeRef& ty) override
        {
            if(auto* e = ty.data_mut().opt_Array())
            {
                this->visit_type( e->inner );
                DEBUG("Array size " << ty);
                if( auto* se1 = e->size.opt_Unevaluated() ) {
                    if( auto* se = se1->opt_Unevaluated() ) {
                        check(::HIR::TypeRef(::HIR::CoreType::Usize), ::HIR::Function::args_t {}, **se);
                    }
                }
            }
            else {
                ::HIR::Visitor::visit_type(ty);
            }
        }
        // ------
        // Code-containing items
        // ------
        void visit_function(::HIR::ItemPath p, ::HIR::Function& item) override {
            if( item.m_code )
            {
                DEBUG("Function code " << p);
                auto _ = this->m_resolve.set_item_generics(item.m_params);
                check(item.m_return, item.m_args, item.m_code);
            }
            else
            {
                DEBUG("Function code " << p << " (none)");
            }
        }
        void visit_static(::HIR::ItemPath p, ::HIR::Static& item) override {
            if( item.m_value )
            {
                check(item.m_type, ::HIR::Function::args_t(), item.m_value);
            }
        }
        void visit_constant(::HIR::ItemPath p, ::HIR::Constant& item) override {
            if( item.m_value )
            {
                check(item.m_type, ::HIR::Function::args_t(), item.m_value);
            }
            m_resolve.expand_associated_types(Span(), item.m_type);
        }
        void visit_enum(::HIR::ItemPath p, ::HIR::Enum& item) override {
            //auto _ = this->m_ms.set_item_generics(item.m_params);

            if( auto* e = item.m_data.opt_Value() )
            {
                auto enum_type = ::HIR::Enum::get_repr_type(item.m_tag_repr);
                for(auto& var : e->variants)
                {
                    DEBUG("Enum value " << p << " - " << var.name);

                    if( var.expr )
                    {
                        check(enum_type, ::HIR::Function::args_t(), var.expr);
                    }
                }
            }
        }

        void visit_trait(::HIR::ItemPath p, ::HIR::Trait& item) override
        {
            TRACE_FUNCTION_F("trait " << p);
            auto _ = this->m_resolve.set_impl_generics(item.m_params);
            ::HIR::Visitor::visit_trait(p, item);
        }
        void visit_type_impl(::HIR::TypeImpl& impl) override
        {
            TRACE_FUNCTION_F("impl " << impl.m_type);
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);
            ::HIR::Visitor::visit_type_impl(impl);
        }
        void visit_trait_impl(const ::HIR::SimplePath& trait_path, ::HIR::TraitImpl& impl) override
        {
            TRACE_FUNCTION_F("impl" << impl.m_params.fmt_args() << " " << trait_path << " for " << impl.m_type);
            auto _ = this->m_resolve.set_impl_generics(impl.m_params);
            ::HIR::Visitor::visit_trait_impl(trait_path, impl);
        }
    };
}

void HIR_Expand_LifetimeInfer(::HIR::Crate& crate)
{
    OuterVisitor    ov(crate);
    ov.visit_crate( crate );
}

void HIR_Expand_LifetimeInfer_Expr(const ::HIR::Crate& crate, const ::HIR::ItemPath& ip, const HIR::Function::args_t& args, const HIR::TypeRef& ret_ty, ::HIR::ExprPtr& exp)
{
    TRACE_FUNCTION_F("ip=" << ip << " ret_ty=" << ret_ty << ", args=" << args);
    StaticTraitResolve  resolve { crate };
    if(exp.m_state->m_impl_generics)   resolve.set_impl_generics(*exp.m_state->m_impl_generics);
    if(exp.m_state->m_item_generics)   resolve.set_item_generics(*exp.m_state->m_item_generics);

    HIR_Expand_LifetimeInfer_ExprInner(resolve, args, ret_ty, exp);
}