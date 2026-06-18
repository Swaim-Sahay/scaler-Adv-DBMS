// Lab 4 — B-Tree of minimum degree T (Part 2 of the lab)
//
// A B-Tree of minimum degree T stores between T-1 and 2T-1 keys
// per node (root may hold as few as 1). With T=2 each internal
// node has 2, 3, or 4 children — that gives a small, well-shaped
// tree for the demo. In PostgreSQL/MySQL, B-Tree nodes are 8 KB
// pages, so one disk read fetches hundreds of keys.
//
// Insertion is "split on the way down": before descending into a
// full child (2T-1 keys), we split it, promote the median key,
// and continue. This guarantees the recursion never lands on a
// full node.
//
// Deletion is more delicate — it can underflow a node. Before
// descending we call fill() to ensure the target child has at
// least T keys: borrow from a sibling with >=T keys, otherwise
// merge with a sibling. Inside the node we either erase directly
// (leaf) or replace with predecessor/successor + recurse.

#include <iostream>
#include <vector>

const int T = 2; // Minimum degree; raise for higher fanout

struct BNode
{
    std::vector<int> keys;
    std::vector<BNode*> children;
    bool leaf = true;

    BNode() = default;
};

class BTree
{
    BNode* root = nullptr;

    // Split child[i] of parent. child must be full (2T-1 keys).
    // Median key is promoted; right half of keys + children become
    // a new sibling node z.
    void split_child(BNode* parent, int i)
    {
        BNode* y = parent->children[i];
        BNode* z = new BNode();

        z->leaf = y->leaf;

        // Right half of y's keys → z
        z->keys.assign(y->keys.begin() + T, y->keys.end());

        int med = y->keys[T - 1];

        y->keys.resize(T - 1);

        if (!y->leaf)
        {
            z->children.assign(
                y->children.begin() + T,
                y->children.end()
            );

            y->children.resize(T);
        }

        parent->keys.insert(parent->keys.begin() + i, med);

        parent->children.insert(
            parent->children.begin() + i + 1,
            z
        );
    }

    void insert_non_full(BNode* node, int key)
    {
        int i = node->keys.size() - 1;

        if (node->leaf)
        {
            node->keys.push_back(0);

            while (i >= 0 && key < node->keys[i])
            {
                node->keys[i + 1] = node->keys[i];
                i--;
            }

            node->keys[i + 1] = key;
        }
        else
        {
            while (i >= 0 && key < node->keys[i])
                i--;

            i++;

            if ((int)node->children[i]->keys.size() == 2 * T - 1)
            {
                split_child(node, i);

                if (key > node->keys[i])
                    i++;
            }

            insert_non_full(node->children[i], key);
        }
    }

    int get_predecessor(BNode* node, int idx)
    {
        BNode* cur = node->children[idx];

        while (!cur->leaf)
            cur = cur->children.back();

        return cur->keys.back();
    }

    int get_successor(BNode* node, int idx)
    {
        BNode* cur = node->children[idx + 1];

        while (!cur->leaf)
            cur = cur->children.front();

        return cur->keys.front();
    }

    // Merge children[idx] and children[idx+1] around the separator
    // key keys[idx]. Used when neither sibling has a key to lend.
    void merge(BNode* node, int idx)
    {
        BNode* left = node->children[idx];
        BNode* right = node->children[idx + 1];

        left->keys.push_back(node->keys[idx]);

        left->keys.insert(
            left->keys.end(),
            right->keys.begin(),
            right->keys.end()
        );

        if (!left->leaf)
        {
            left->children.insert(
                left->children.end(),
                right->children.begin(),
                right->children.end()
            );
        }

        node->keys.erase(node->keys.begin() + idx);
        node->children.erase(node->children.begin() + idx + 1);

        delete right;
    }

    // Ensure children[idx] has at least T keys before we descend
    // into it. Borrow from a richer sibling if possible, else merge.
    void fill(BNode* node, int idx)
    {
        if (idx > 0 &&
            (int)node->children[idx - 1]->keys.size() >= T)
        {
            // Borrow from left sibling
            BNode* child = node->children[idx];
            BNode* sibling = node->children[idx - 1];

            child->keys.insert(
                child->keys.begin(),
                node->keys[idx - 1]
            );

            node->keys[idx - 1] = sibling->keys.back();
            sibling->keys.pop_back();

            if (!child->leaf)
            {
                child->children.insert(
                    child->children.begin(),
                    sibling->children.back()
                );

                sibling->children.pop_back();
            }
        }
        else if (
            idx < (int)node->children.size() - 1 &&
            (int)node->children[idx + 1]->keys.size() >= T)
        {
            // Borrow from right sibling
            BNode* child = node->children[idx];
            BNode* sibling = node->children[idx + 1];

            child->keys.push_back(node->keys[idx]);

            node->keys[idx] = sibling->keys.front();

            sibling->keys.erase(sibling->keys.begin());

            if (!child->leaf)
            {
                child->children.push_back(
                    sibling->children.front()
                );

                sibling->children.erase(
                    sibling->children.begin()
                );
            }
        }
        else
        {
            // Neither sibling can lend → merge
            if (idx < (int)node->children.size() - 1)
                merge(node, idx);
            else
                merge(node, idx - 1);
        }
    }

    void delete_key(BNode* node, int key)
    {
        int idx = 0;

        while (
            idx < (int)node->keys.size() &&
            key > node->keys[idx]
        )
            idx++;

        if (
            idx < (int)node->keys.size() &&
            node->keys[idx] == key
        )
        {
            // Key is in this node
            if (node->leaf)
            {
                node->keys.erase(
                    node->keys.begin() + idx
                );
            }
            else if (
                (int)node->children[idx]->keys.size() >= T
            )
            {
                int pred = get_predecessor(node, idx);

                node->keys[idx] = pred;

                delete_key(
                    node->children[idx],
                    pred
                );
            }
            else if (
                (int)node->children[idx + 1]->keys.size() >= T
            )
            {
                int succ = get_successor(node, idx);

                node->keys[idx] = succ;

                delete_key(
                    node->children[idx + 1],
                    succ
                );
            }
            else
            {
                merge(node, idx);
                delete_key(node->children[idx], key);
            }
        }
        else
        {
            if (node->leaf)
            {
                std::cout << "Key not found\n";
                return;
            }

            bool last =
                (idx == (int)node->children.size());

            if (
                (int)node->children[last ? idx - 1 : idx]
                        ->keys.size() < T
            )
            {
                fill(node, last ? idx - 1 : idx);
            }

            if (
                last &&
                idx > (int)node->keys.size()
            )
            {
                delete_key(
                    node->children[idx - 1],
                    key
                );
            }
            else
            {
                delete_key(
                    node->children[idx],
                    key
                );
            }
        }
    }

    void inorder(BNode* node) const
    {
        if (!node)
            return;

        for (int i = 0; i < (int)node->keys.size(); i++)
        {
            if (!node->leaf)
                inorder(node->children[i]);

            std::cout << node->keys[i] << " ";
        }

        if (!node->leaf)
            inorder(node->children.back());
    }

public:
    void insert(int key)
    {
        if (!root)
        {
            root = new BNode();
            root->keys.push_back(key);
            return;
        }

        if ((int)root->keys.size() == 2 * T - 1)
        {
            BNode* s = new BNode();

            s->leaf = false;
            s->children.push_back(root);

            split_child(s, 0);

            root = s;
        }

        insert_non_full(root, key);
    }

    void remove(int key)
    {
        if (!root)
            return;

        delete_key(root, key);

        if (root->keys.empty() && !root->leaf)
        {
            BNode* old = root;

            root = root->children[0];

            delete old;
        }
    }

    bool search(BNode* node, int key) const
    {
        int i = 0;

        while (
            i < (int)node->keys.size() &&
            key > node->keys[i]
        )
            i++;

        if (
            i < (int)node->keys.size() &&
            node->keys[i] == key
        )
            return true;

        if (node->leaf)
            return false;

        return search(node->children[i], key);
    }

    bool search(int key) const
    {
        return root && search(root, key);
    }

    void print() const
    {
        inorder(root);
        std::cout << "\n";
    }
};

int main()
{
    BTree bt;

    int values[] = {
        10, 20, 5, 6, 12,
        30, 7, 17, 3, 1, 25
    };

    for (int k : values)
        bt.insert(k);

    std::cout << "Inorder after inserts:\n";
    bt.print();

    std::cout << "Search 17: "
              << (bt.search(17) ? "found" : "not found")
              << "\n";

    std::cout << "Search 99: "
              << (bt.search(99) ? "found" : "not found")
              << "\n";

    bt.remove(6);
    bt.remove(20);

    std::cout
        << "Inorder after removing 6 and 20:\n";

    bt.print();

    return 0;
}
