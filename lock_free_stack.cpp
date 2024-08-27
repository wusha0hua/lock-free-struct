#include <climits>
#include <cstddef>
#include <iostream>
#include <atomic>
#include <memory>
#include <ostream>
#include <random>
#include <ctime>
#include <stdexcept>
#include <thread>
#include <vector>
#include <chrono>
#include <exception>

template<typename T>
class LockFreeStack {
public:
	LockFreeStack() {}
	LockFreeStack(const LockFreeStack&) = delete;
	LockFreeStack& operator=(const LockFreeStack&) = delete;
	virtual void push(const T& data) = 0;
	virtual std::shared_ptr<T> pop() = 0;
	bool empty() {
		return (this->head.load() == nullptr);
	}
	size_t size() {
		return this->size_.load();
	}
protected:
	struct Node {
		std::shared_ptr<T> data;
		Node *next;
		Node(const T& data) {
			this->data = std::make_shared<T>(data);
		}
	};
	std::atomic<Node*> head;
	std::atomic<size_t> size_;
};

template<typename T>
class LockFreeStackCount final: public LockFreeStack<T> {
	using Node = typename LockFreeStack<T>::Node;
public:
	LockFreeStackCount() {
		this->head.store(nullptr);
		this->threads_in_pop.store(0);
		to_be_deleted.store(nullptr);
		this->size_.store(0);
	}
	~LockFreeStackCount() {
		while(pop());
	}
	void push(const T& data) {
		Node *new_node = new Node(data);
		while(!this->head.compare_exchange_weak(new_node->next, new_node));
		this->size_.fetch_add(1);
	}	
	std::shared_ptr<T> pop() {
		this->threads_in_pop.fetch_add(1);
		Node *old_node;
		do {
			old_node = this->head.load();
			if(old_node == nullptr) {
				this->threads_in_pop.fetch_sub(1);
				return std::shared_ptr<T>();
			}
		} while(!this->head.compare_exchange_weak(old_node, old_node->next));
		std::shared_ptr<T> ret;
		if(old_node) {
			ret.swap(old_node->data);
			old_node->next = nullptr;
		}
		try_delete(old_node);
		this->size_.fetch_sub(1);
		return ret;
	}
private:
	std::atomic<int> threads_in_pop;
	std::atomic<Node*> to_be_deleted;
	void try_delete(Node *node) {
		if(this->threads_in_pop.load() == 1) {
			Node *nodes = to_be_deleted.exchange(nullptr);	
			if(!--threads_in_pop) {
				while(nodes != nullptr) {
					Node *tmp = nodes;
					nodes = nodes->next;
					//std::cout << "delete node" << std::endl;
					delete tmp;
				}
			} else if(nodes) {
				insert_to_delete(nodes);	
			}
			//std::cout << "delete node" << std::endl;
			delete node;
		} else {
			insert_to_delete(node);
			this->threads_in_pop.fetch_sub(1);
		}
	}
	void insert_to_delete(Node *node) {
		Node *head = node;
		Node *tail = node;
		while(node != nullptr) {
			tail = node;
			node = node->next;
		}
		while(!to_be_deleted.compare_exchange_weak(tail->next, head));
	}
};

template<typename T>
class LockFreeStackHazardPointer final: public LockFreeStack<T> {
	using Node = typename LockFreeStack<T>::Node;
public:
	LockFreeStackHazardPointer() {
		this->head.store(nullptr);
		this->size_.store(0);
		to_be_deleted.store(nullptr);
	}
	~LockFreeStackHazardPointer() {
		while(pop());
	}
	void push(const T& data) {
		Node *new_node = new Node(data);
		while(!this->head.compare_exchange_weak(new_node->next, new_node));
	}	
	std::shared_ptr<T> pop() {
		std::atomic<void*>& hazard_pointer = get_hazard_pointer_for_current_thread();
		Node *old_head = this->head.load();
		do {
			hazard_pointer.store(old_head);	
		} while(old_head && !this->head.compare_exchange_weak(old_head, old_head->next));
		hazard_pointer.store(nullptr);
		std::shared_ptr<T> ret = std::shared_ptr<T>();
		if(old_head) {
			ret.swap(old_head->data);
			if(outstanding_hazard_pointer_for(old_head)) {
				insert_to_delete(old_head);
			} else {
				delete old_head;
			}
			delete_nodes_with_no_hazard();
		}
		return ret;
	}
private:
	struct HazardPointer {
		std::atomic<std::thread::id> id;
		std::atomic<void*> pointer;
		HazardPointer() {
			id = std::thread::id();
			pointer = nullptr;
		}
	};
	class HazardPointerManager {
	public:
		HazardPointerManager() {
			hp = nullptr;
			for(int i = 0; i < hazard_pointers_nums; i++) {
				std::thread::id old_id;
				if(hazard_pointers[i].id.compare_exchange_strong(old_id, std::this_thread::get_id())) {
					hp = &hazard_pointers[i];
					break;
				}
			}	
			if(hp == nullptr) throw std::runtime_error("no hazard pointer available");
		}
		~HazardPointerManager() {
			hp->id.store(std::thread::id());
			hp->pointer.store(nullptr);
		}
		std::atomic<void*>& get_pointer() {
			return hp->pointer;
		}
	private:
		HazardPointer *hp;
	};
	static constexpr size_t hazard_pointers_nums = 20;
	static HazardPointer hazard_pointers[hazard_pointers_nums];
	std::atomic<Node*> to_be_deleted;

	std::atomic<void*>& get_hazard_pointer_for_current_thread() {
		thread_local HazardPointerManager hazard_manager;
		return hazard_manager.get_pointer();
	} 
	bool outstanding_hazard_pointer_for(void *p) {
		for(int i = 0; i < hazard_pointers_nums; i++) {
			if(hazard_pointers[i].pointer.load() == p) {
				return true;
			}
		}
		return false;
	}
	void insert_to_delete(Node *node) {
		node->next = to_be_deleted.load();
		while(!to_be_deleted.compare_exchange_weak(node->next, node));
	}
	void delete_nodes_with_no_hazard() {
		Node *current = to_be_deleted.exchange(nullptr);
		while(current) {
			Node *next = current->next;
			if(outstanding_hazard_pointer_for(current)) {
				insert_to_delete(current);
			} else {
				delete current;
			}
			current = next;
		}
	}
};

template<typename T>
typename LockFreeStackHazardPointer<T>::HazardPointer LockFreeStackHazardPointer<T>::hazard_pointers[LockFreeStackHazardPointer<T>::hazard_pointers_nums];

template<typename T>
class LockFreeStackReference final: public LockFreeStack<T> {
public:
	LockFreeStackReference() {
	}
	~LockFreeStackReference() {
		while(pop());
	}
	void push(const T& data) {
		RefNode new_head;
		new_head.node_ptr = new Node(data);
		new_head.outer_ref = 1;
		new_head.node_ptr->next = head.load();
		while(!head.compare_exchange_weak(new_head.node_ptr->next, new_head));
		/*
		RefNode new_head = RefNode(data);
		new_head.node_ptr->next = head.load();
		while(!head.compare_exchange_strong(new_head.node_ptr->next, new_head));
		*/
	}
	std::shared_ptr<T> pop() {
		RefNode old_head = head.load();
		while(true) {
			RefNode new_head;
			do {
				new_head = old_head;
				new_head.outer_ref++;
			} while(!head.compare_exchange_weak(old_head, new_head));
			old_head = new_head;
			Node *node_ptr = old_head.node_ptr;
			if(node_ptr == nullptr) return std::shared_ptr<T>();
			if(head.compare_exchange_strong(old_head, node_ptr->next)) {
				std::shared_ptr<T> ret;
				ret.swap(node_ptr->data);
				int thread_count = old_head.outer_ref - 2;
				if(node_ptr->inner_ref.fetch_add(thread_count) == -thread_count) {
					delete node_ptr;
				}
				return ret;	
			} else {
				if(node_ptr->inner_ref.fetch_sub(1) == 1) {
					delete node_ptr;
				}
			}
		}
	}
private:
	struct RefNode;
	struct Node {
		std::shared_ptr<T> data;
		std::atomic<int> inner_ref;	
		RefNode next;	
		Node(const T& data): data(std::make_shared<T>(data)), inner_ref(0) {}
	};
	struct RefNode {
		Node *node_ptr;
		int outer_ref;
		RefNode(): node_ptr(nullptr), outer_ref(1) {}
		RefNode(const T& data): node_ptr(new Node(data)), outer_ref(0) {}
	};
	std::atomic<RefNode> head;
};

class TestClass {
public:
	TestClass(int id, std::string name): id(id), name(name) {
	}
	~TestClass() {
	}
	int id;
	std::string name;
	static std::default_random_engine e;
	static TestClass random_test_class() {
		int len = e() % 20;
		std::string name;
		for(int i = 0; i < len; i++) name.push_back((e() % (127 - 33)) + 33);
		return std::move(TestClass{static_cast<int>(e()), std::move(name)});
	}
	friend std::ostream& operator<<(std::ostream& out, const TestClass& tc) {
		out << "TestClass {" << "id: " <<tc.id << ", name: " << tc.name  << "}";
		return out;
	}
};
std::default_random_engine TestClass::e(time(nullptr));

template<typename T>
void test_lock_free_stack(LockFreeStack<T>& stack) {
	auto thread_to_push = [&stack]() {
		for(unsigned long long i = 0; i < 1000000; i++) {
			stack.push(TestClass::random_test_class());	
			//std::this_thread::sleep_for(std::chrono::microseconds(1));
		}
		std::cout << "push finish" << std::endl;
	};
	auto thread_to_pop = [&stack](int id) {
		while(true) {
			auto ret = stack.pop();
			std::this_thread::sleep_for(std::chrono::microseconds(10));
		}
	};
	int push_num = std::thread::hardware_concurrency() / 2;
	int pop_num = std::thread::hardware_concurrency() - push_num;
	std::vector<std::thread> threads;
	for(int i = 0; i < push_num; i++) {
		threads.emplace_back(std::thread(thread_to_push));
	}
	for(int i = 0; i < pop_num; i++) {
		threads.emplace_back(std::thread(thread_to_pop, i));
	}
	for(int i = 0; i < threads.size(); i++) {
		threads[i].join();
	}
}


int main() {
	LockFreeStackReference<TestClass> stack;
	stack.push(TestClass::random_test_class());
}

