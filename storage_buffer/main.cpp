#include <iostream>
#include <vector>
#include <string>

using namespace std;

template<typename Key, typename Row>
class BTreeHelper {
private:
    struct Entry {
        Key key;
        Row row;
    };

    struct BTree {
        vector<Entry> keys;
        vector<BTree*> children;

        bool isLeaf = true;
    };

    BTree* root;

    size_t minDegree;
    size_t maxDegree;
    size_t minKeys;
    size_t maxKeys;

private:
    Entry* searchNode(BTree* node, Key key) {
        int i = 0;

        while (i < node->keys.size() && key > node->keys[i].key) {
            i++;
        }

        if (i < node->keys.size() && key == node->keys[i].key) {
            return &node->keys[i];
        }

        if (node->isLeaf) {
            return nullptr;
        }

        return searchNode(node->children[i], key);
    }

    void splitChild(BTree* parent, int childIndex) {
        BTree* fullChild = parent->children[childIndex];

        BTree* newNode = new BTree();
        newNode->isLeaf = fullChild->isLeaf;

        int t = minDegree;

        Entry middleKey = fullChild->keys[t - 1];

        for (int i = t; i < fullChild->keys.size(); i++) {
            newNode->keys.push_back(fullChild->keys[i]);
        }

        fullChild->keys.resize(t - 1);

        if (!fullChild->isLeaf) {

            for (int i = t; i < fullChild->children.size(); i++) {
                newNode->children.push_back(fullChild->children[i]);
            }

            fullChild->children.resize(t);
        }

        parent->children.insert(
            parent->children.begin() + childIndex + 1,
            newNode
        );

        parent->keys.insert(
            parent->keys.begin() + childIndex,
            middleKey
        );
    }

    void insertNonFull(BTree* node, Key key, Row row) {
        int i = node->keys.size() - 1;

        if (node->isLeaf) {

            Entry entry{key, row};

            node->keys.push_back(entry);

            while (i >= 0 && key < node->keys[i].key) {
                node->keys[i + 1] = node->keys[i];
                i--;
            }

            node->keys[i + 1] = entry;
        }
        else {

            while (i >= 0 && key < node->keys[i].key) {
                i--;
            }

            i++;

            if (node->children[i]->keys.size() == maxKeys) {

                splitChild(node, i);

                if (key > node->keys[i].key) {
                    i++;
                }
            }

            insertNonFull(node->children[i], key, row);
        }
    }

    void printNode(BTree* node, int level) {

        cout << "Level " << level << ": ";

        for (auto& entry : node->keys) {
            cout << entry.key << " ";
        }

        cout << endl;

        if (!node->isLeaf) {
            for (auto child : node->children) {
                printNode(child, level + 1);
            }
        }
    }

public:

    BTreeHelper(size_t degree) {

        root = new BTree();

        minDegree = degree;
        maxDegree = 2 * degree;

        minKeys = minDegree - 1;
        maxKeys = maxDegree - 1;
    }

    Entry* search(Key key) {
        return searchNode(root, key);
    }

    void insert(Key key, Row row) {
        if (search(key) != nullptr) {
            return;
        }

        if (root->keys.size() == maxKeys) {
            BTree* newRoot = new BTree();

            newRoot->isLeaf = false;

            newRoot->children.push_back(root);

            splitChild(newRoot, 0);

            root = newRoot;
        }

        insertNonFull(root, key, row);
    }

    void print() {
        printNode(root, 0);
    }
};

int main() {

    BTreeHelper<int, string> tree(3);

    tree.insert(10, "A");
    tree.insert(20, "B");
    tree.insert(5, "C");
    tree.insert(6, "D");
    tree.insert(12, "E");
    tree.insert(30, "F");
    tree.insert(7, "G");
    tree.insert(17, "H");

    tree.print();

    auto result = tree.search(12);

    if (result != nullptr) {
        cout << "\nFound: "
             << result->key
             << " -> "
             << result->row
             << endl;
    }
    else {
        cout << "\nKey not found" << endl;
    }

    return 0;
}