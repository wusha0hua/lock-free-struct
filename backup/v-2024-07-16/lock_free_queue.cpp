#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include <thread>
#include <atomic>

class TestClass {
public:
	TestClass(): id(0) {}
	TestClass(int id): id(id) {}
	int id;
};

template<typename T, size_t size>
class LockCircleQueue: std::allocator<T> {
public:
	LockCircleQueue() {
		data = std::allocator<T>::allocate(size + 1);
		head = 0;
		tail = 0;
		capacity = size + 1;
	}
	~LockCircleQueue() {
		std::unique_lock<std::mutex> lock(queue_mutex);
		while(head != tail) {
			std::allocator<T>::destroy(data + head);
			head = (head + 1) % capacity;
		}
		std::allocator<T>::deallocate(data, capacity);
	}
	bool empty() {
		std::unique_lock<std::mutex> lock(queue_mutex);
		return head == tail;
	}
	bool full() {
		std::unique_lock<std::mutex> lock(queue_mutex);
		return (tail + 1) % capacity == head;
	}
	bool push(T&& element) {
		std::unique_lock<std::mutex> lock(queue_mutex);
		if((tail + 1) % capacity == head) return false;
		std::allocator<T>::construct(data + tail, element);
		tail = (tail + 1) % capacity;
		return true;
	}
	bool pop(T& element) {
		std::unique_lock<std::mutex> lock(queue_mutex);
		if(tail == head) return false;
		element = std::move(data[head]);
		head = (head + 1) % capacity;
		return true;
	}
private:
	size_t head;
	size_t tail;
	size_t capacity;
	T* data;
	std::mutex queue_mutex;
};

template<typename T, size_t size>
class LockFreeCircleQueueSpin: std::allocator<T> {
public:
	LockFreeCircleQueueSpin() {
		capacity = size + 1;
		head = 0;
		tail = 0;
		atomic_using = false;
		data = std::allocator<T>::allocate(capacity);
	}
	~LockFreeCircleQueueSpin() {
		bool use_expected = false;
		bool use_desired = true;
		do {
			use_expected = false;
			use_desired = true;
		} while(!atomic_using.compare_exchange_strong(use_expected, use_desired));
		while(head != tail) {
			std::allocator<T>::destroy(data + head);
			head = (head + 1) % capacity;
		}
		std::allocator<T>::deallocate(data, capacity);
		do {
			use_expected = true;
			use_desired = false;
		} while(!atomic_using.compare_exchange_strong(use_expected, use_desired));
	}
	bool push(T&& element) {
		bool use_expected = false;
		bool use_desired = true;
		do {
			use_expected = false;
			use_desired = true;
		} while(!atomic_using.compare_exchange_strong(use_expected, use_desired));
		if((tail + 1) % capacity == head) {
			do {
				use_expected = true;
				use_desired = false;
			} while(!atomic_using.compare_exchange_strong(use_expected, use_desired));
			return false;
		}
		std::allocator<T>::construct(data + tail, element);
		tail = (tail + 1) % capacity;
		do {
			use_expected = true;
			use_desired = false;
		} while(!atomic_using.compare_exchange_strong(use_expected, use_desired));
		return true;
	}
	bool pop(T& element) {
		bool use_expected = false;
		bool use_desired = true;
		do {
			use_expected = false;
			use_desired = true;
		} while(!atomic_using.compare_exchange_strong(use_expected, use_desired));
		if(tail == head) {
			do {
				use_expected = true;
				use_desired = false;
			} while(!atomic_using.compare_exchange_strong(use_expected, use_desired));
			return false;
		}
		element = std::move(data[head]);
		head = (head + 1) % capacity;
		do {
			use_expected = true;
			use_desired = false;
		} while(!atomic_using.compare_exchange_strong(use_expected, use_desired));
		return true;
	}
	bool empty() {
		bool flag;
		bool use_expected = false;
		bool use_desired = true;
		do {
			use_expected = false;
			use_desired = true;
		} while(!atomic_using.compare_exchange_strong(use_expected, use_desired));
		if(head == tail) flag = true;
		else flag = false;
		do {
			use_expected = true;
			use_desired = false;
		} while(!atomic_using.compare_exchange_strong(use_expected, use_desired));
		return flag;
	}
private:
	size_t capacity;
	size_t head;
	size_t tail;
	T* data;
	std::atomic<bool> atomic_using;
};

template<typename T, size_t size>
class LockFreeCircleQueue: std::allocator<T> {
public:
	LockFreeCircleQueue() {
		head = 0;
		tail = 0;
		tail_update = 0;
		capacity = size + 1;
		data = std::allocator<T>::allocate(capacity);
	}
	~LockFreeCircleQueue() {
		while(head != tail) {
			std::allocator<T>::destroy(data + head);
			head = (head + 1) % capacity;
		}
		std::allocator<T>::deallocate(data, capacity);
	}
	bool push(T&& element) {
		size_t t;
		do {
			t = tail.load();
			if((t + 1) % capacity == head.load()) return false;
		} while(!tail.compare_exchange_strong(t, (t + 1) % capacity));
		data[t] = element;
		size_t tup;
		do {
			tup = t;
		} while(tail_update.compare_exchange_strong(tup, (tup + 1) % capacity));
		return true;
	}
	bool pop(T& element) {
		size_t h;
		do {
			h = head.load();
			if(h == tail.load()) return false;
			if(h == tail_update.load()) return false;
			element = data[h];
		} while(!head.compare_exchange_strong(h, (h + 1) % capacity));
		return true;
	}
	bool empty() {
		return head.load() == tail.load();
	}
private:
	size_t capacity;
	T* data;
	std::atomic<size_t> head;
	std::atomic<size_t> tail;
	std::atomic<size_t> tail_update; 
};

void test_lock_circle_queue() {
	int n = 100;
	std::vector<TestClass> vec;
	for(int i = 0; i < n; i++) vec.emplace_back(TestClass(i));
	LockCircleQueue<TestClass, 10> queue;
	std::mutex print_mutex;
	bool quit = false;
	auto push_to_queue = [&queue, &print_mutex, &quit](std::vector<TestClass>&& vec) {
		for(int i = 0; i < vec.size(); i++) {
			while(queue.push(std::move(vec[i])) == false) {
				std::unique_lock<std::mutex> lock(print_mutex);
				std::cout << "push (" << i << "): queue full"<< std::endl;
			}
			std::unique_lock<std::mutex> lock(print_mutex);
			std::cout << "push (" << i << "): finished" << std::endl;
		}	
		quit = true;
	};
	auto pop_from_queue = [&queue, &print_mutex, &quit]() {
		TestClass tc;
		while(!quit || !queue.empty()) {
			if(queue.empty() == true) {
				continue;
			}
			queue.pop(tc);
			std::unique_lock<std::mutex> lock(print_mutex);
			std::cout << "thread " << std::this_thread::get_id() << ": pop (" << tc.id << ") " << std::endl; 
		}
		std::unique_lock<std::mutex> lock(print_mutex);
		std::cout << "thread " << std::this_thread::get_id() << ": quit" << std::endl;
	};
	std::vector<std::thread> threads;
	threads.emplace_back(std::thread(push_to_queue, std::move(vec)));
	for(int i = 0; i < std::thread::hardware_concurrency() - 1; i++) {
		threads.emplace_back(std::thread(pop_from_queue));
	}
	for(int i = 0; i < threads.size(); i++) threads[i].join();
}

void test_lock_free_circle_queue() {
	int n = 100;
	std::vector<TestClass> vec;
	for(int i = 0; i < n; i++) vec.emplace_back(TestClass(i));
	LockFreeCircleQueueSpin<TestClass, 10> queue;
	std::mutex print_mutex;
	bool quit = false;
	auto push_to_queue = [&queue, &print_mutex, &quit](std::vector<TestClass>&& vec) {
		for(int i = 0; i < vec.size(); i++) {
			while(queue.push(std::move(vec[i])) == false) {
				std::unique_lock<std::mutex> lock(print_mutex);
				std::cout << "push (" << i << "): queue full"<< std::endl;
			}
			std::unique_lock<std::mutex> lock(print_mutex);
			std::cout << "push (" << i << "): finished" << std::endl;
		}	
		quit = true;
	};
	auto pop_from_queue = [&queue, &print_mutex, &quit]() {
		TestClass tc;
		while(!quit || !queue.empty()) {
			if(queue.empty() == true) {
				continue;
			}
			queue.pop(tc);
			std::unique_lock<std::mutex> lock(print_mutex);
			std::cout << "thread " << std::this_thread::get_id() << ": pop (" << tc.id << ") " << std::endl; 
		}
		std::unique_lock<std::mutex> lock(print_mutex);
		std::cout << "thread " << std::this_thread::get_id() << ": quit" << std::endl;
	};
	std::vector<std::thread> threads;
	threads.emplace_back(std::thread(push_to_queue, std::move(vec)));
	for(int i = 0; i < std::thread::hardware_concurrency() - 1; i++) {
		threads.emplace_back(std::thread(pop_from_queue));
	}
	for(int i = 0; i < threads.size(); i++) threads[i].join();
}


int main() {
	test_lock_free_circle_queue();
}
