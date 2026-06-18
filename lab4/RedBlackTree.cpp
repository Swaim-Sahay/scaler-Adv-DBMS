// Lab 4 — Red-Black Tree (Part 1 of the lab)
//
// A self-balancing BST. Every node is RED or BLACK and the tree
// maintains the 4 invariants:
//   1. Root is BLACK
//   2. No two consecutive RED nodes (a RED node's parent is BLACK)
//   3. Every path from a node to its NULL descendants has the same
//      number of BLACK nodes ("black-height")
// Together these guarantee O(log n) height.
//
// Insert is the standard top-down BST insert followed by fix_insert,
// which handles 3 cases via recoloring and rotations:
//   Case 1: uncle is RED        → recolor parent + uncle BLACK,
//                                  grandparent RED, recurse on grandparent
//   Case 2: triangle (LR / RL)  → rotate parent to straighten into Case 3
//   Case 3: line     (LL / RR)  → rotate grandparent, swap colors
//
// Delete uses CLRS-style transplant + minimum-of-right-subtree
// successor, then fix_delete to restore black-height when a BLACK
// node was removed.

#include <iostream>

enum Color
{
    RED,
    BLACK
};

struct RBNode
{
    int key;
    Color color;
    RBNode *left, *right, *parent;

    explicit RBNode(int k)
        : key(k), color(RED), left(nullptr), right(nullptr), parent(nullptr)
    {
    }
};

class RedBlackTree
{
    RBNode *root = nullptr;

    void left_rotate(RBNode *x)
    {
        RBNode *y = x->right;

        x->right = y->left;
        if (y->left)
            y->left->parent = x;

        y->parent = x->parent;

        if (!x->parent)
            root = y;
        else if (x == x->parent->left)
            x->parent->left = y;
        else
            x->parent->right = y;

        y->left = x;
        x->parent = y;
    }

    void right_rotate(RBNode *x)
    {
        RBNode *y = x->left;

        x->left = y->right;
        if (y->right)
            y->right->parent = x;

        y->parent = x->parent;

        if (!x->parent)
            root = y;
        else if (x == x->parent->right)
            x->parent->right = y;
        else
            x->parent->left = y;

        y->right = x;
        x->parent = y;
    }

    void fix_insert(RBNode *z)
    {
        while (z->parent && z->parent->color == RED)
        {
            RBNode *gp = z->parent->parent;

            if (z->parent == gp->left)
            {
                RBNode *uncle = gp->right;

                if (uncle && uncle->color == RED)
                {
                    // Case 1: uncle RED — recolor and recurse on grandparent
                    z->parent->color = BLACK;
                    uncle->color = BLACK;
                    gp->color = RED;
                    z = gp;
                }
                else
                {
                    if (z == z->parent->right)
                    {
                        // Case 2 (LR): straighten to LL
                        z = z->parent;
                        left_rotate(z);
                    }

                    // Case 3 (LL): rotate grandparent, swap colors
                    z->parent->color = BLACK;
                    gp->color = RED;
                    right_rotate(gp);
                }
            }
            else
            {
                // Mirror image: parent is right child of grandparent
                RBNode *uncle = gp->left;

                if (uncle && uncle->color == RED)
                {
                    z->parent->color = BLACK;
                    uncle->color = BLACK;
                    gp->color = RED;
                    z = gp;
                }
                else
                {
                    if (z == z->parent->left)
                    {
                        z = z->parent;
                        right_rotate(z);
                    }

                    z->parent->color = BLACK;
                    gp->color = RED;
                    left_rotate(gp);
                }
            }
        }

        root->color = BLACK;
    }

    void transplant(RBNode *u, RBNode *v)
    {
        if (!u->parent)
            root = v;
        else if (u == u->parent->left)
            u->parent->left = v;
        else
            u->parent->right = v;

        if (v)
            v->parent = u->parent;
    }

    RBNode *minimum(RBNode *node)
    {
        while (node->left)
            node = node->left;

        return node;
    }

    void fix_delete(RBNode *x, RBNode *x_parent)
    {
        while (x != root && (!x || x->color == BLACK))
        {
            if (x == (x_parent ? x_parent->left : nullptr))
            {
                RBNode *w = x_parent->right;

                if (w && w->color == RED)
                {
                    w->color = BLACK;
                    x_parent->color = RED;
                    left_rotate(x_parent);
                    w = x_parent->right;
                }

                if ((!w->left || w->left->color == BLACK) &&
                    (!w->right || w->right->color == BLACK))
                {
                    if (w)
                        w->color = RED;

                    x = x_parent;
                    x_parent = x->parent;
                }
                else
                {
                    if (!w->right || w->right->color == BLACK)
                    {
                        if (w->left)
                            w->left->color = BLACK;

                        w->color = RED;
                        right_rotate(w);
                        w = x_parent->right;
                    }

                    w->color = x_parent->color;
                    x_parent->color = BLACK;

                    if (w->right)
                        w->right->color = BLACK;

                    left_rotate(x_parent);
                    x = root;
                }
            }
            else
            {
                RBNode *w = x_parent->left;

                if (w && w->color == RED)
                {
                    w->color = BLACK;
                    x_parent->color = RED;
                    right_rotate(x_parent);
                    w = x_parent->left;
                }

                if ((!w->right || w->right->color == BLACK) &&
                    (!w->left || w->left->color == BLACK))
                {
                    if (w)
                        w->color = RED;

                    x = x_parent;
                    x_parent = x->parent;
                }
                else
                {
                    if (!w->left || w->left->color == BLACK)
                    {
                        if (w->right)
                            w->right->color = BLACK;

                        w->color = RED;
                        left_rotate(w);
                        w = x_parent->left;
                    }

                    w->color = x_parent->color;
                    x_parent->color = BLACK;

                    if (w->left)
                        w->left->color = BLACK;

                    right_rotate(x_parent);
                    x = root;
                }
            }
        }

        if (x)
            x->color = BLACK;
    }

    void inorder(RBNode *node) const
    {
        if (!node)
            return;

        inorder(node->left);
        std::cout << node->key
                  << (node->color == RED ? "R" : "B")
                  << " ";
        inorder(node->right);
    }

public:
    void insert(int key)
    {
        RBNode *z = new RBNode(key);
        RBNode *y = nullptr;
        RBNode *x = root;

        while (x)
        {
            y = x;
            x = (z->key < x->key) ? x->left : x->right;
        }

        z->parent = y;

        if (!y)
            root = z;
        else if (z->key < y->key)
            y->left = z;
        else
            y->right = z;

        fix_insert(z);
    }

    void remove(int key)
    {
        RBNode *z = root;

        while (z && z->key != key)
        {
            z = (key < z->key) ? z->left : z->right;
        }

        if (!z)
            return;

        RBNode *y = z;
        RBNode *x = nullptr;
        RBNode *x_parent = nullptr;

        Color y_orig_color = y->color;

        if (!z->left)
        {
            x = z->right;
            x_parent = z->parent;
            transplant(z, z->right);
        }
        else if (!z->right)
        {
            x = z->left;
            x_parent = z->parent;
            transplant(z, z->left);
        }
        else
        {
            y = minimum(z->right);
            y_orig_color = y->color;
            x = y->right;

            if (y->parent == z)
            {
                x_parent = y;
            }
            else
            {
                x_parent = y->parent;

                transplant(y, y->right);

                y->right = z->right;
                y->right->parent = y;
            }

            transplant(z, y);

            y->left = z->left;
            y->left->parent = y;

            y->color = z->color;
        }

        delete z;

        if (y_orig_color == BLACK)
            fix_delete(x, x_parent);
    }

    void print() const
    {
        inorder(root);
        std::cout << "\n";
    }
};

int main()
{
    RedBlackTree rbt;

    int values[] = {10, 20, 30, 15, 25, 5, 1};

    for (int k : values)
        rbt.insert(k);

    std::cout << "Inorder (key + color R/B):\n";
    rbt.print();

    rbt.remove(20);

    std::cout << "After removing 20:\n";
    rbt.print();

    return 0;
}
