#include <atomic>
#include <exception>
#include <iostream>
#include <thread>
#include <mutex>
#include <memory>
#include <stack>

struct empty_stack: std::exception {
	const char *what() const throw();
};

template<typename T>
class LockThreadSafeStack {
public:
	LockThreadSafeStack() {}
	LockThreadSafeStack(const LockThreadSafeStack& other) {
		std::lock_guard<std::mutex> lock(mtx);
		data = other.data;
	}
	LockThreadSafeStack& operator=(const LockThreadSafeStack&) = delete; 
	void push(T&& element) {
		std::lock_guard<std::mutex> lock(mtx);
		data.emplace(std::move(element));
	} 
	std::shared_ptr<T> pop() {
		std::lock_guard<std::mutex> lock(mtx);
		if(data.empty()) throw empty_stack();
		auto p = std::shared_ptr<T>(std::move(data.top()));
		data.pop();
		return p;
	}
	bool empty() {
		std::lock_guard<std::mutex> lock(mtx);
		return data.empty();
	}
private:
	std::stack<T> data;
	mutable std::mutex mtx;
};

template<typename T>
class Node {
public:
	std::shared_ptr<T> data;
	Node *next;
	Node(const T& data_) {
		data = std::make_shared<T>(data_);
		next = nullptr;
	} 
};

template<typename T>
class LockTreeStack {
public:
	LockTreeStack() {
		threads_in_pop.store(0);
	}
	void push(const T& value) {
		Node<T>* new_node = new Node(value);
		do {
			new_node->next = head.load();
		} while(!head.compare_exchange_strong(new_node->next, new_node));
	}
	std::shared_ptr<T> pop() {
		threads_in_pop.fetch_add(1);
		Node<T> *old_head;
		do {
			old_head = head.load();
			if(old_head == nullptr) {
				threads_in_pop.fetch_sub(1);
				return std::shared_ptr<T>();
			}
		}while(!head.compare_exchange_strong(old_head, old_head->next));
		std::shared_ptr<T> res;
		if(old_head != nullptr) {
			res.swap(old_head->data);
		}
		//try_delete(old_head);	
		return res;
	}
	void try_delete(Node<T>* node) {
		if(threads_in_pop.load() == 1) {
			Node<T> *node_to_delete = to_be_deleted.exchange(nullptr);
			if(!--threads_in_pop) {
				delete_nodes(node_to_delete);
			} else if(node_to_delete) {
				chain_pending_node(node_to_delete);
			}
			delete node;
		} else {
			chain_pending_node(node);
			threads_in_pop.fetch_sub(1);
		}
	}
	static void delete_nodes(Node<T>* nodes) {
		while(nodes) {
			Node<T> *next = nodes->next;
			delete nodes;
			nodes = next;
		}
	}
	void chain_pending_node(Node<T> *node) {
		chain_pending_nodes(node, node);
	}
	void chain_pending_nodes(Node<T> *first, Node<T>* last) {
		last->next = to_be_deleted;
		while (!to_be_deleted.compare_exchange_weak(last->next, first));
	}
private:

	std::atomic<Node<T>*> head;
	std::atomic<int> threads_in_pop;
	std::atomic<Node<T>*> to_be_deleted;
};

class TestClass {
public:
	TestClass(int id): id(id) {}
	int& get_id() {
		return id;
	}
	void set_id(int id) {
		id = id;
	}
private:
	int id;
};

void test_lock_thread_safe_stack() {
	LockThreadSafeStack<TestClass> stack;
}

void test_lock_free_stack() {
	LockTreeStack<TestClass> stack;
	std::allocator<TestClass> alc;
	TestClass *tcs = alc.allocate(10);
	for(int i = 0; i < 10; i++) alc.construct(tcs + i, i);
	//for(int i = 0; i < 10; i++) std::cout << tcs[i].get_id() << std::endl;
	for(int i = 0; i < 10; i++) stack.push(tcs[i]);	
	for(int i = 0; i < 10; i++) std::cout << stack.pop()->get_id() << std::endl;
}

int main() {
	//test_lock_thread_safe_stack();
	test_lock_free_stack();
}
