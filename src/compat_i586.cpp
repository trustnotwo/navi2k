// Pentium / Pentium MMX support: stand-ins for the pieces of the prebuilt C++ runtime that
// the player uses. The MinGW-w64 toolchain's libstdc++, libgcc and CRT were compiled for the
// Pentium Pro and contain CMOV/FCOMI, which crash a Pentium. Defining these here, compiled
// with -march=i586, keeps the linker from pulling in the library versions at all:
//
//   - operator new/delete, which would also pull in the whole C++ exception machinery
//   - the std::__throw_* helpers the templates call (the player is built without exceptions)
//   - the red-black tree behind std::map and std::set
//   - _pei386_runtime_relocator, from the CRT startup
//   - __strtod, MinGW's C99 strtod
//
// std::string/std::wstring come from src/lp_i586_cxxconfig.h, which makes every file
// instantiate them itself. cmake/check_binary.cmake (SCAN_CMOV) fails the build if any
// P6-only instruction remains anywhere in the .exe.

#include <windows.h>
#include <stdlib.h>
#include <new>
#include <map>

namespace {

__attribute__((noreturn)) void Fatal(const char* what)
{
    OutputDebugStringA(what);
    MessageBoxA(NULL, what, "Navi2K Player", MB_ICONERROR | MB_OK);
    abort();
}

void* Allocate(std::size_t n)
{
    void* p = malloc(n ? n : 1);
    if (!p)
        Fatal("Out of memory.");
    return p;
}

} // namespace

// --- operator new / delete ---------------------------------------------------------------
// Out of memory ends the program (there are no exceptions to throw).

void* operator new(std::size_t n) { return Allocate(n); }
void* operator new[](std::size_t n) { return Allocate(n); }
void* operator new(std::size_t n, const std::nothrow_t&) throw() { return malloc(n ? n : 1); }
void* operator new[](std::size_t n, const std::nothrow_t&) throw() { return malloc(n ? n : 1); }
void operator delete(void* p) throw() { free(p); }
void operator delete[](void* p) throw() { free(p); }
void operator delete(void* p, const std::nothrow_t&) throw() { free(p); }
void operator delete[](void* p, const std::nothrow_t&) throw() { free(p); }
void operator delete(void* p, std::size_t) throw() { free(p); }
void operator delete[](void* p, std::size_t) throw() { free(p); }

// --- std::__throw_* -------------------------------------------------------------------------
// Called by the containers on misuse (an out-of-range index, a string over max_size). Built
// without exceptions, the only thing to do is stop.

namespace std {
void __throw_bad_alloc() { Fatal("Out of memory."); }
void __throw_bad_array_new_length() { Fatal("Out of memory (array size)."); }
void __throw_length_error(const char* s) { Fatal(s); }
void __throw_logic_error(const char* s) { Fatal(s); }
void __throw_out_of_range(const char* s) { Fatal(s); }
void __throw_out_of_range_fmt(const char* s, ...) { Fatal(s); }
void __throw_invalid_argument(const char* s) { Fatal(s); }
void __throw_domain_error(const char* s) { Fatal(s); }
void __throw_overflow_error(const char* s) { Fatal(s); }
void __throw_underflow_error(const char* s) { Fatal(s); }
void __throw_range_error(const char* s) { Fatal(s); }
void __throw_runtime_error(const char* s) { Fatal(s); }
void __throw_bad_cast() { Fatal("Bad cast."); }
void __throw_bad_typeid() { Fatal("Bad typeid."); }
void __throw_bad_exception() { Fatal("Bad exception."); }
void __throw_bad_function_call() { Fatal("Bad function call."); }
} // namespace std

// --- std::map / std::set: the red-black tree ------------------------------------------------
// The classic algorithm (Cormen et al.; the same one libstdc++ uses, from the SGI STL), on
// libstdc++'s node layout. The header node's parent is the root, its left the leftmost
// node and its right the rightmost; it is red, which tells it apart from the root.

namespace std {
namespace {

typedef _Rb_tree_node_base Node;

inline bool IsRed(const Node* n) { return n && n->_M_color == _S_red; }

Node* Minimum(Node* x)
{
    while (x->_M_left)
        x = x->_M_left;
    return x;
}

Node* Maximum(Node* x)
{
    while (x->_M_right)
        x = x->_M_right;
    return x;
}

void RotateLeft(Node* x, Node*& root)
{
    Node* y = x->_M_right;
    x->_M_right = y->_M_left;
    if (y->_M_left)
        y->_M_left->_M_parent = x;
    y->_M_parent = x->_M_parent;
    if (x == root)
        root = y;
    else if (x == x->_M_parent->_M_left)
        x->_M_parent->_M_left = y;
    else
        x->_M_parent->_M_right = y;
    y->_M_left = x;
    x->_M_parent = y;
}

void RotateRight(Node* x, Node*& root)
{
    Node* y = x->_M_left;
    x->_M_left = y->_M_right;
    if (y->_M_right)
        y->_M_right->_M_parent = x;
    y->_M_parent = x->_M_parent;
    if (x == root)
        root = y;
    else if (x == x->_M_parent->_M_right)
        x->_M_parent->_M_right = y;
    else
        x->_M_parent->_M_left = y;
    y->_M_right = x;
    x->_M_parent = y;
}

Node* Next(Node* x)
{
    if (x->_M_right)
        return Minimum(x->_M_right);
    Node* y = x->_M_parent;
    while (x == y->_M_right) {
        x = y;
        y = y->_M_parent;
    }
    // Stepping past the rightmost node of a one-node tree climbs to the header, whose right
    // is that node again: stay on the header (end()).
    return x->_M_right != y ? y : x;
}

Node* Prev(Node* x)
{
    if (x->_M_color == _S_red && x->_M_parent->_M_parent == x)
        return x->_M_right;   // end() steps back to the rightmost node
    if (x->_M_left)
        return Maximum(x->_M_left);
    Node* y = x->_M_parent;
    while (x == y->_M_left) {
        x = y;
        y = y->_M_parent;
    }
    return y;
}

} // namespace

_Rb_tree_node_base* _Rb_tree_increment(_Rb_tree_node_base* x) throw() { return Next(x); }
const _Rb_tree_node_base* _Rb_tree_increment(const _Rb_tree_node_base* x) throw()
{
    return Next(const_cast<Node*>(x));
}
_Rb_tree_node_base* _Rb_tree_decrement(_Rb_tree_node_base* x) throw() { return Prev(x); }
const _Rb_tree_node_base* _Rb_tree_decrement(const _Rb_tree_node_base* x) throw()
{
    return Prev(const_cast<Node*>(x));
}

void _Rb_tree_insert_and_rebalance(const bool insertLeft, _Rb_tree_node_base* x, _Rb_tree_node_base* p,
                                   _Rb_tree_node_base& header) throw()
{
    Node*& root = header._M_parent;
    x->_M_parent = p;
    x->_M_left = 0;
    x->_M_right = 0;
    x->_M_color = _S_red;

    if (insertLeft) {
        p->_M_left = x;   // also makes leftmost = x when p is the header
        if (p == &header) {
            header._M_parent = x;
            header._M_right = x;
        } else if (p == header._M_left) {
            header._M_left = x;
        }
    } else {
        p->_M_right = x;
        if (p == header._M_right)
            header._M_right = x;
    }

    while (x != root && x->_M_parent->_M_color == _S_red) {
        Node* const grand = x->_M_parent->_M_parent;
        if (x->_M_parent == grand->_M_left) {
            Node* const uncle = grand->_M_right;
            if (IsRed(uncle)) {
                x->_M_parent->_M_color = _S_black;
                uncle->_M_color = _S_black;
                grand->_M_color = _S_red;
                x = grand;
            } else {
                if (x == x->_M_parent->_M_right) {
                    x = x->_M_parent;
                    RotateLeft(x, root);
                }
                x->_M_parent->_M_color = _S_black;
                grand->_M_color = _S_red;
                RotateRight(grand, root);
            }
        } else {
            Node* const uncle = grand->_M_left;
            if (IsRed(uncle)) {
                x->_M_parent->_M_color = _S_black;
                uncle->_M_color = _S_black;
                grand->_M_color = _S_red;
                x = grand;
            } else {
                if (x == x->_M_parent->_M_left) {
                    x = x->_M_parent;
                    RotateRight(x, root);
                }
                x->_M_parent->_M_color = _S_black;
                grand->_M_color = _S_red;
                RotateLeft(grand, root);
            }
        }
    }
    root->_M_color = _S_black;
}

_Rb_tree_node_base* _Rb_tree_rebalance_for_erase(_Rb_tree_node_base* const z, _Rb_tree_node_base& header) throw()
{
    Node*& root = header._M_parent;
    Node*& leftmost = header._M_left;
    Node*& rightmost = header._M_right;
    Node* y = z;
    Node* x = 0;
    Node* xParent = 0;

    if (!y->_M_left) {
        x = y->_M_right;   // z has at most one child; x may be null
    } else if (!y->_M_right) {
        x = y->_M_left;    // z has exactly one child
    } else {
        y = Minimum(y->_M_right);   // two children: y is z's successor
        x = y->_M_right;
    }

    if (y != z) {
        // Put y in z's place; z keeps y's color and is the node to free.
        z->_M_left->_M_parent = y;
        y->_M_left = z->_M_left;
        if (y != z->_M_right) {
            xParent = y->_M_parent;
            if (x)
                x->_M_parent = y->_M_parent;
            y->_M_parent->_M_left = x;
            y->_M_right = z->_M_right;
            z->_M_right->_M_parent = y;
        } else {
            xParent = y;
        }
        if (root == z)
            root = y;
        else if (z->_M_parent->_M_left == z)
            z->_M_parent->_M_left = y;
        else
            z->_M_parent->_M_right = y;
        y->_M_parent = z->_M_parent;
        _Rb_tree_color c = y->_M_color;
        y->_M_color = z->_M_color;
        z->_M_color = c;
        y = z;
    } else {
        xParent = y->_M_parent;
        if (x)
            x->_M_parent = y->_M_parent;
        if (root == z)
            root = x;
        else if (z->_M_parent->_M_left == z)
            z->_M_parent->_M_left = x;
        else
            z->_M_parent->_M_right = x;
        if (leftmost == z)
            leftmost = z->_M_right ? Minimum(x) : z->_M_parent;   // z->_M_left is null here
        if (rightmost == z)
            rightmost = z->_M_left ? Maximum(x) : z->_M_parent;   // z->_M_right is null here
    }

    if (y->_M_color != _S_red) {
        while (x != root && !IsRed(x)) {
            if (x == xParent->_M_left) {
                Node* w = xParent->_M_right;
                if (w->_M_color == _S_red) {
                    w->_M_color = _S_black;
                    xParent->_M_color = _S_red;
                    RotateLeft(xParent, root);
                    w = xParent->_M_right;
                }
                if (!IsRed(w->_M_left) && !IsRed(w->_M_right)) {
                    w->_M_color = _S_red;
                    x = xParent;
                    xParent = xParent->_M_parent;
                } else {
                    if (!IsRed(w->_M_right)) {
                        w->_M_left->_M_color = _S_black;
                        w->_M_color = _S_red;
                        RotateRight(w, root);
                        w = xParent->_M_right;
                    }
                    w->_M_color = xParent->_M_color;
                    xParent->_M_color = _S_black;
                    if (w->_M_right)
                        w->_M_right->_M_color = _S_black;
                    RotateLeft(xParent, root);
                    break;
                }
            } else {
                Node* w = xParent->_M_left;
                if (w->_M_color == _S_red) {
                    w->_M_color = _S_black;
                    xParent->_M_color = _S_red;
                    RotateRight(xParent, root);
                    w = xParent->_M_left;
                }
                if (!IsRed(w->_M_right) && !IsRed(w->_M_left)) {
                    w->_M_color = _S_red;
                    x = xParent;
                    xParent = xParent->_M_parent;
                } else {
                    if (!IsRed(w->_M_left)) {
                        w->_M_right->_M_color = _S_black;
                        w->_M_color = _S_red;
                        RotateLeft(w, root);
                        w = xParent->_M_left;
                    }
                    w->_M_color = xParent->_M_color;
                    xParent->_M_color = _S_black;
                    if (w->_M_left)
                        w->_M_left->_M_color = _S_black;
                    RotateRight(xParent, root);
                    break;
                }
            }
        }
        if (x)
            x->_M_color = _S_black;
    }
    return y;
}

} // namespace std

// --- CRT startup -------------------------------------------------------------------------------
// The CRT applies "pseudo relocations" (data imported from DLLs without dllimport) at startup;
// its version contains CMOV. The player has none, so this checks that and does nothing.

extern "C" char __RUNTIME_PSEUDO_RELOC_LIST__[];
extern "C" char __RUNTIME_PSEUDO_RELOC_LIST_END__[];

extern "C" void _pei386_runtime_relocator(void)
{
    // An empty list, or just the 12-byte version-2 header.
    if (__RUNTIME_PSEUDO_RELOC_LIST_END__ - __RUNTIME_PSEUDO_RELOC_LIST__ > 12)
        Fatal("This build needs runtime pseudo-relocations, which the Pentium build doesn't support.");
}

// --- strtod ----------------------------------------------------------------------------------
// MinGW's headers send C's strtod (cJSON's number parser) to its own C99 version, __strtod,
// built for i686. msvcrt.dll's strtod reads every number JSON can hold.

extern "C" double (__cdecl* const lp_msvcrt_strtod)(const char*, char**) __asm__("__imp__strtod");
extern "C" double __cdecl lp_strtod(const char* s, char** end) __asm__("___strtod");
extern "C" double __cdecl lp_strtod(const char* s, char** end)
{
    return lp_msvcrt_strtod(s, end);
}
