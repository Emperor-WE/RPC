#ifndef SKIPLIST_H
#define SKIPLIST_H

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>

#define STORE_FILE "store/dumpFile"

static std::string delimiter = ":";

/**
 * @brief 跳表节点 模板
 * @details 节点为key-value型，由于跳表为多层结构，数组forward用于存放每层中该节点的下一节点
 */
template <typename K, typename V>
class Node
{
public:
    Node() {}

    Node(K k, V v, int);

    ~Node();

    K get_key() const;

    V get_value() const;

    void set_value(V);

    /* 存放不同层中下一个节点的指针的线性表【竖着看，类似邻接表】*/
    Node<K, V> **forward;

    /* 层级（node_level），表示节点在跳表中有多少层 */
    int node_level;

private:
    K key;
    V value;
};

/**
 * @brief 构造函数
 * @param[in] level level为该节点在跳表中有多少层，level由创建节点时随机分配
 */
template <typename K, typename V>
Node<K, V>::Node(const K k, const V v, int level)
{
    this->key = k;
    this->value = v;
    this->node_level = level;

    // level + 1, because array index is from 0 - level
    this->forward = new Node<K, V> *[level + 1];

    // Fill forward array with 0(NULL)
    memset(this->forward, 0, sizeof(Node<K, V> *) * (level + 1));
}

/**
 * @brief 释放单个节点空间
 */
template <typename K, typename V>
Node<K, V>::~Node()
{
    delete[] forward;
}

/**
 * @brief 获取节点 key 值
 */
template <typename K, typename V>
K Node<K, V>::get_key() const
{
    return key;
}

/**
 * @brief 获取节点 value 值
 */
template <typename K, typename V>
V Node<K, V>::get_value() const
{
    return value;
}

/**
 * @brief 设置节点 value 值
 */
template <typename K, typename V>
void Node<K, V>::set_value(V value)
{
    this->value = value;
}

/**
 * @brief 用于序列化和反序列化跳表中的数据到文件或字符串
 */
template <typename K, typename V>
class SkipListDump
{
public:
    friend class boost::serialization::access;

    template <class Archive>
    void serialize(Archive &ar, const unsigned int version)
    {
        ar & keyDumpVt_;
        ar & valDumpVt_;
    }
    std::vector<K> keyDumpVt_;
    std::vector<V> valDumpVt_;

public:
    void insert(const Node<K, V> &node);
};

/**
 * @brief 跳表定义
 * @details 管理跳表的整体结构，包括插入、删除、搜索等操作
 */
template <typename K, typename V>
class SkipList
{
public:
    SkipList(int);
    ~SkipList();

    int get_random_level();

    Node<K, V> *create_node(K, V, int);

    int insert_element(K, V);

    void display_list();

    bool search_element(K, V &value);

    void delete_element(K);

    void insert_set_element(K &, V &);

    std::string dump_file();

    void load_file(const std::string &dumpStr);

    void clear(Node<K, V> *);

    int size();

private:
    void get_key_value_from_string(const std::string &str, std::string *key, std::string *value);
    bool is_valid_string(const std::string &str);

private:
    
    int _max_level;         // 跳表最大层数
    int _skip_list_level;   // 跳表当前已存储元素的最大层数
    int _element_count;     // 跳表当前元素个数

    Node<K, V> *_header;    // 跳表头节点（不存储数据）

    std::ofstream _file_writer; // 文件写入流
    std::ifstream _file_reader; // 文件读取流

    std::mutex _mtx;    // 临界区互斥锁
};

/**
 * @brief 创建一个节点
 * @param[in] level 该节点的最大层数
 */ 
template <typename K, typename V>
Node<K, V> *SkipList<K, V>::create_node(const K k, const V v, int level)
{
    Node<K, V> *n = new Node<K, V>(k, v, level);
    return n;
}

// Insert given key and value in skip list
// return 1 means element exists
// return 0 means insert successfully
/*
                           +------------+
                           |  insert 50 |
                           +------------+
level 4     +-->1+                                                      100
                 |
                 |                      insert +----+
level 3         1+-------->10+---------------> | 50 |          70       100
                                               |    |
                                               |    |
level 2         1          10         30       | 50 |          70       100
                                               |    |
                                               |    |
level 1         1    4     10         30       | 50 |          70       100
                                               |    |
                                               |    |
level 0         1    4   9 10         30   40  | 50 |  60      70       100
                                               +----+

*/

/**
 * @brief 插入操作
 * @details 
 *      1.需要在每层中找到插入的位置（即每层插入位置的前一个节点，保存在 update 中）
 *      2.key已存在（不管value是否相同），直接返回【注意区分 insert_set_element方法】
 *      3.获取待插入节点所随机的层数 
 *          a)该节点层数 > 当前跳表层数，高于的层数部分的 update 指针域指向头节点，表示是该层的第一个节点
 *      4.对于 update 所记录的插入位置，以及待插入节点的层数，将节点依此插入每一层
 */
template <typename K, typename V>
int SkipList<K, V>::insert_element(const K key, const V value)
{
    _mtx.lock();
    Node<K, V> *current = this->_header;

    // update[i]是第i层中key最后一个比插入key小的node*
    Node<K, V> *update[_max_level + 1];
    memset(update, 0, sizeof(Node<K, V> *) * (_max_level + 1));

    // 从最高层搜索填补update
    for (int i = _skip_list_level; i >= 0; i--)
    {
        while (current->forward[i] != NULL && current->forward[i]->get_key() < key)
        {
            current = current->forward[i];
        }
        update[i] = current;
    }

    // 第0层, current->forward[0]为应该插入的位置
    current = current->forward[0];

    // 该key已存在，解锁后直接返回
    if (current != NULL && current->get_key() == key)
    {
        std::cout << "key: " << key << ", exists" << std::endl;
        _mtx.unlock();
        return 1;
    }

    // current为空，表示到达了该层的末尾， 不为空则要在update[0]和current之间插入
    // 随机层数比当前的层数高时，比当前层高的层前一节点就是_header
    if (current == NULL || current->get_key() != key)
    {
        // 随机层数
        int random_level = get_random_level();

        // 随机层数比当前的层数高时，比当前层高的层前一节点就是_header
        if (random_level > _skip_list_level)
        {
            for (int i = _skip_list_level + 1; i < random_level + 1; i++)
            {
                update[i] = _header;
            }
            _skip_list_level = random_level;
        }

        // create new node with random level generated
        // 创建节点
        Node<K, V> *inserted_node = create_node(key, value, random_level);

        // 插入节点
        //1、对每一层，插入节点的下一节点为update[i]的下一节点
        //2、update[i]的下一节点更新为插入节点
        for (int i = 0; i <= random_level; i++)
        {
            inserted_node->forward[i] = update[i]->forward[i];
            update[i]->forward[i] = inserted_node;
        }
        std::cout << "Successfully inserted key:" << key << ", value:" << value << std::endl;
        _element_count++;   // 增加节点数
    }
    _mtx.unlock();
    return 0;
}

/**
 * @brief 输出当前跳表（从第0层开始）
 */
template <typename K, typename V>
void SkipList<K, V>::display_list()
{
    std::cout << "\n*****Skip List*****" << "\n";

    for (int i = 0; i <= _skip_list_level; i++)
    {
        Node<K, V> *node = this->_header->forward[i];
        std::cout << "Level " << i << ": ";
        while (node != NULL)
        {
            std::cout << node->get_key() << ":" << node->get_value() << ";";
            node = node->forward[i];
        }
        std::cout << std::endl;
    }
}

// TODO 对dump 和 load 后面可能要考虑加锁的问题
// 序列化（将内存中的数据转储到文件中）
template <typename K, typename V>
std::string SkipList<K, V>::dump_file()
{
    // std::cout << "dump_file-----------------" << std::endl;

    // _file_writer.open(STORE_FILE);
    Node<K, V> *node = this->_header->forward[0];
    SkipListDump<K, V> dumper;
    while (node != nullptr)
    {
        dumper.insert(*node);
        // _file_writer << node->get_key() << ":" << node->get_value() << "\n";
        // std::cout << node->get_key() << ":" << node->get_value() << ";\n";
        node = node->forward[0];
    }
    std::stringstream ss;
    boost::archive::text_oarchive oa(ss);
    oa << dumper;
    return ss.str();
    // _file_writer.flush();
    // _file_writer.close();
}

// 反序列化（从磁盘中加载数据）
template <typename K, typename V>
void SkipList<K, V>::load_file(const std::string &dumpStr)
{
    // _file_reader.open(STORE_FILE);
    // std::cout << "load_file-----------------" << std::endl;
    // std::string line;
    // std::string* key = new std::string();
    // std::string* value = new std::string();
    // while (getline(_file_reader, line)) {
    //     get_key_value_from_string(line, key, value);
    //     if (key->empty() || value->empty()) {
    //         continue;
    //     }
    //     // Define key as int type
    //     insert_element(stoi(*key), *value);
    //     std::cout << "key:" << *key << "value:" << *value << std::endl;
    // }
    // delete key;
    // delete value;
    // _file_reader.close();

    if (dumpStr.empty())
    {
        return;
    }
    SkipListDump<K, V> dumper;
    std::stringstream iss(dumpStr);
    boost::archive::text_iarchive ia(iss);
    ia >> dumper;
    for (int i = 0; i < dumper.keyDumpVt_.size(); ++i)
    {
        insert_element(dumper.keyDumpVt_[i], dumper.keyDumpVt_[i]);
    }
}

/**
 * @brief 获取当前跳表元素个数
 */
template <typename K, typename V>
int SkipList<K, V>::size()
{
    return _element_count;
}

/**
 * @brief 拆分字符串为 key & value
 * @param[in] str 待拆分字符串，必须是 "key:value" 形式
 * @param[out] key 拆分后的 key
 * @param[out] value 拆分后的 value
 */
template <typename K, typename V>
void SkipList<K, V>::get_key_value_from_string(const std::string &str, std::string *key, std::string *value)
{
    if (!is_valid_string(str))
    {
        return;
    }
    *key = str.substr(0, str.find(delimiter));
    *value = str.substr(str.find(delimiter) + 1, str.length());
}

/**
 * @brief 判断 str 是否有效，必须是 "key:value" 形式
 */
template <typename K, typename V>
bool SkipList<K, V>::is_valid_string(const std::string &str)
{
    if (str.empty())
    {
        return false;
    }
    if (str.find(delimiter) == std::string::npos)
    {
        return false;
    }
    return true;
}

/**
 * @brief 从跳表中删除节点
 * @param[in] key 待删除节点的 key 值
 * @details 类似于插入操作，首先定位要删除的节点，然后更新前驱节点的forward指针以绕过被删除的节点
 */
template <typename K, typename V>
void SkipList<K, V>::delete_element(K key)
{
    _mtx.lock();
    Node<K, V> *current = this->_header;
    Node<K, V> *update[_max_level + 1];
    memset(update, 0, sizeof(Node<K, V> *) * (_max_level + 1));

    // 从跳表最高层节点开始查找
    for (int i = _skip_list_level; i >= 0; i--)
    {
        while (current->forward[i] != NULL && current->forward[i]->get_key() < key)
        {
            current = current->forward[i];
        }
        update[i] = current;
    }

    // 获取待删除的节点（赋值前，curent->forward[0] == update[0]，皆为待删除节点的前驱节点）
    current = current->forward[0];
    /* 待删除节点存在 */
    if (current != NULL && current->get_key() == key)
    {
        // 从第 0 层开始，删除每个层的待删除节点
        // TODO 改进，可以直接获取待删除节点的有效层数，无需从 _skip_list_level 判断
        for (int i = 0; i <= _skip_list_level; i++)
        {
            // 对于第 i 层，若不存在待删除节点，退出循环
            if (update[i]->forward[i] != current)
                break;

            update[i]->forward[i] = current->forward[i];
        }

        // 最顶层不在拥有节点（即待删除节点在顶层也存在，且是唯一节点），则更新跳表层数
        while (_skip_list_level > 0 && _header->forward[_skip_list_level] == 0)
        {
            _skip_list_level--;
        }

        std::cout << "Successfully deleted key " << key << std::endl;
        delete current;
        _element_count--;
    }
    _mtx.unlock();
    return;
}

/**
 * @brief 作用与insert_element相同类似，
 * @note insert_element是插入新元素，
 * @note insert_set_element是插入元素，如果元素存在则改变其值
 */
template <typename K, typename V>
void SkipList<K, V>::insert_set_element(K &key, V &value)
{
    V oldValue;
    if (search_element(key, oldValue))
    {
        delete_element(key);
    }
    insert_element(key, value);
}

// Search for element in skip list
/*
                           +------------+
                           |  select 60 |
                           +------------+
level 4     +-->1+                                                      100
                 |
                 |
level 3         1+-------->10+------------------>50+           70       100
                                                   |
                                                   |
level 2         1          10         30         50|           70       100
                                                   |
                                                   |
level 1         1    4     10         30         50|           70       100
                                                   |
                                                   |
level 0         1    4   9 10         30   40    50+-->60      70       100
*/
template <typename K, typename V>
bool SkipList<K, V>::search_element(K key, V &value)
{
    std::cout << "search_element-----------------" << std::endl;
    Node<K, V> *current = _header;

    // 从跳跃表的最高层开始
    for (int i = _skip_list_level; i >= 0; i--)
    {
        while (current->forward[i] && current->forward[i]->get_key() < key)
        {
            current = current->forward[i];
        }
    }

    // 到达0层并将指针移动到我们搜索的右节点
    current = current->forward[0];

    // 如果当前节点的键值等于搜索的键值，我们就得到它
    if (current && current->get_key() == key)
    {
        value = current->get_value();
        std::cout << "Found key: " << key << ", value: " << current->get_value() << std::endl;
        return true;
    }

    std::cout << "Not Found Key:" << key << std::endl;
    return false;
}

template <typename K, typename V>
void SkipListDump<K, V>::insert(const Node<K, V> &node)
{
    keyDumpVt_.emplace_back(node.get_key());
    valDumpVt_.emplace_back(node.get_value());
}

/**
 * @brief 跳表构造函数
 * @param[in] max_level 跳表最大层数
 */
template <typename K, typename V>
SkipList<K, V>::SkipList(int max_level)
{
    this->_max_level = max_level;
    this->_skip_list_level = 0;
    this->_element_count = 0;

    // 创建头节点并将键和值初始化为null
    K k;
    V v;
    this->_header = new Node<K, V>(k, v, _max_level);
};

/* TODO ： 项目中的析构函数应该是有内存泄漏的问题的，只detele了头节点 */
template <typename K, typename V>
SkipList<K, V>::~SkipList()
{
    if (_file_writer.is_open())
    {
        _file_writer.close();
    }
    if (_file_reader.is_open())
    {
        _file_reader.close();
    }

    // 递归删除跳表链条
    if (_header->forward[0] != nullptr)
    {
        clear(_header->forward[0]);
    }
    delete (_header);
}

/**
 * @brief 递归删除节点
 */
template <typename K, typename V>
void SkipList<K, V>::clear(Node<K, V> *cur)
{
    if (cur->forward[0] != nullptr)
    {
        clear(cur->forward[0]);
    }
    delete (cur);
}

/**
 * @brief 计算节点随机层数
 */
template <typename K, typename V>
int SkipList<K, V>::get_random_level()
{
    int k = 1;
    while (rand() % 2)
    {
        k++;
    }
    k = (k < _max_level) ? k : _max_level;
    return k;
}

// vim: et tw=100 ts=4 sw=4 cc=120

#endif // SKIPLIST_H
