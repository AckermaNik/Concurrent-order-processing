#include <cstddef>
#include <cstdio>
#include <string>
#include <iostream>
#include <pthread.h>
#include <unistd.h> 
#include <time.h>
#include <memory>
#include <variant>
#include <random>
#include <chrono>
#include <atomic>
#include <limits>
#include <sys/types.h>

#ifndef N_THREADS
#define N_THREADS 4  // Default value, if Makefile does not provide any
#endif
#include <cassert>
#define DIST (N_THREADS / 2) 
#define AGNT (N_THREADS / 4)
#define COOK (N_THREADS / 4)
#define EMPTY_STACK -2
#define EMPTY_QUEUE -2
#define CAPACITY (N_THREADS / 4)
#define EMPTY 1
#define WAITING 2
#define BUSY 3
#define TIMEOUT -1
#define PUSH_VALID -10
#define DUMMY -50

static_assert(N_THREADS > 0);
static_assert((N_THREADS % 4) == 0);

using Order = int;
using namespace std;  // Brings all names from the 'std' namespace into scope

struct stack_node {
    Order order_id;
    stack_node* next;

    stack_node(Order val) : order_id(val), next(nullptr) {}
};

using ReturnType = variant<std::monostate, stack_node*, int>;

class ExponentialBackoff {
private:
    long min_delay;  // Minimum backoff time (in nanoseconds)
    long max_delay;  // Maximum backoff time (in nanoseconds)
    long current_delay;  // Current backoff time

public:
    ExponentialBackoff(long min_ns = 1000, long max_ns = 1000000)
        : min_delay(min_ns), max_delay(max_ns), current_delay(min_ns) {}

    void backoff() {

        long wait_time = min_delay+(rand() % (max_delay - min_delay + 1));

        struct timespec req;
        req.tv_sec = wait_time / 1000000000;  // Convert ns to seconds
        req.tv_nsec = wait_time % 1000000000; // Get remaining nanoseconds

        nanosleep(&req, nullptr);

        // Double the delay but don't exceed max_delay
        current_delay = min(current_delay * 2, max_delay);
    }

    void reset() {
        // Reset backoff time to minimum
        current_delay = min_delay;
    }
};


class EliminationArray {

    private:

    struct alignas(8) Slot {
            Order value;
            int state;

            Slot() : value(-1), state(EMPTY) {}
    };

    std::atomic<Slot> slots[CAPACITY]; 

    public:
    EliminationArray(){

        for (size_t i = 0; i < CAPACITY; i++)
        {
            slots[i].store( Slot());
        }
    }

    Order exchanger(Order myItem, long timeout_ns) {
        struct timespec deadline,now;
        int slotIndex;

        clock_gettime(CLOCK_REALTIME, &deadline);
        clock_gettime(CLOCK_REALTIME, &now);
        deadline.tv_nsec += timeout_ns;

        // Normalize deadline if nanoseconds exceed 1 second
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_sec += deadline.tv_nsec / 1000000000;
            deadline.tv_nsec %= 1000000000;
        }


        while (true) {

            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
                return TIMEOUT;
            }

            slotIndex = rand() % CAPACITY;
            //printf("slotIndex:%d order id: %d \n",slotIndex,myItem);
            Slot slot = slots[slotIndex];
            Slot new_slot= Slot();
            Slot temp = Slot();
            Slot temp_slot; 
            new_slot.value=myItem;
            new_slot.state=WAITING; 
            

            if (slot.state == EMPTY) {
                if (slots[slotIndex].compare_exchange_strong(slot, new_slot)) {

                    clock_gettime(CLOCK_REALTIME, &now);

                    while (now.tv_sec < deadline.tv_sec ||
                        (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec)) {

                        temp.state=slots[slotIndex].load().state;
                        temp.value=slots[slotIndex].load().value;

                        if (temp.state == BUSY) {
                            //printf("BUSY:%d other:%d ,slotindex:%d \n",myItem, temp.value,slotIndex);
                            temp_slot = slots[slotIndex].load();
                            temp_slot.value = -1;
                            temp_slot.state = EMPTY;
                            slots[slotIndex].store(temp_slot);
                            return temp.value;
                        }

                        clock_gettime(CLOCK_REALTIME, &now);
                    }

                    new_slot.value=-1;
                    new_slot.state=EMPTY;
                    temp.value=myItem;
                    temp.state=WAITING;
                    if (slots[slotIndex].compare_exchange_strong(temp, new_slot)) {
                        return TIMEOUT;
                    } 

                    //the exchange happened at the brick of my timeout and I didnt saw it on time

                    Order value = slots[slotIndex].load().value; 
                    temp_slot = slots[slotIndex].load();
                    temp_slot.value = -1;
                    temp_slot.state = EMPTY;
                    slots[slotIndex].store(temp_slot);
                    return value;
                    
                }

            }
            else if (slot.state == WAITING) {
                Order otherItem = slot.value;
                new_slot.state=BUSY;
                if (slots[slotIndex].compare_exchange_strong(slot, new_slot)) {
                    return otherItem;
                }
            }

            clock_gettime(CLOCK_REALTIME, &now);
        }
    }
};


struct PendingOrdersStack {
    
    std::atomic<stack_node*> top;
    std::atomic<size_t> num_of_elements;
    EliminationArray* array ; //Elimination array

    PendingOrdersStack() : top(nullptr), num_of_elements(0) {array = new EliminationArray();} //constructor

    bool push(Order order_id) {
        stack_node* new_node = new stack_node(order_id);
        ExponentialBackoff Off;
           
            while (1) {

                Off.reset();

                if (tryPush(new_node)) {
                    //fflush(stdout);
                    //printf("push success for: %d\n",order_id);
                    return true;
                }
                
                Order result = array->exchanger(order_id, 2000000000);
                
                // if (result == TIMEOUT) {
                //     std::cout << "Exchange timed out!" << std::endl;
                // }else
                if (result == PUSH_VALID) { // a pop poped my value in the elimination array
                    //std::cout << "Exchanged with pop!"<< order_id << std::endl;
                    return true; //success
                }

                Off.backoff();

            }
    }

    Order pop() {

        ExponentialBackoff Off;

        while (1) {

            Off.reset();
            ReturnType ret=tryPop();


            if (std::holds_alternative<stack_node*>(ret)) {
                stack_node* node = std::get<stack_node*>(ret);
                if (node != nullptr) {
                    //fflush(stdout);
                    //printf("pop success for %d\n",node->tid);
                    return node->order_id;
                }
            }

            Order result = array->exchanger(PUSH_VALID, 2000000000);

            // if (result == TIMEOUT) {
            //     std::cout << "Exchange timed out in pop!\n" << std::endl;
            // }else 
            if (result != TIMEOUT && result != PUSH_VALID) { // exchanged pop with a push
                //printf("Push for pop for:%d \n",result);
                return result;
            }

            Off.backoff();

        }

    }

    bool tryPush(stack_node* new_node) {
        stack_node* oldTop =top.load();
        new_node->next = oldTop;


        //if you wanna see the exchanges use this delay
        //usleep(100000); 
        if (top.compare_exchange_strong(oldTop, new_node)) {
            num_of_elements.fetch_add(1);
            return 1;
        }
        return 0;

    }


    ReturnType tryPop() {
        stack_node* oldTop = top.load(); //std::memory_order_relaxed)
        stack_node* newTop;

        if (oldTop == NULL) {
            return EMPTY_STACK;
        }

        newTop = oldTop->next;
        if (top.compare_exchange_strong(oldTop, newTop)) {
            num_of_elements.fetch_sub(1);
            return oldTop;
        }

        return nullptr;
    }

};

PendingOrdersStack* PendingOrders = new PendingOrdersStack();

struct queue_node {
    Order id;
    queue_node* next;
    queue_node(int val) : id(val), next(nullptr) {}
};

struct UnderPreparationOrdersQueue {
    queue_node* head; 
    queue_node* tail;
    queue_node* dummy; //centinel node
    std::atomic<size_t>  num_of_elements;
    
    pthread_mutex_t head_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t tail_lock = PTHREAD_MUTEX_INITIALIZER;

    UnderPreparationOrdersQueue() : num_of_elements(0){ 

        pthread_mutex_init(&head_lock, nullptr);
        pthread_mutex_init(&tail_lock, nullptr);

        dummy = new queue_node(DUMMY);  // Allocate dummy node
        head = dummy;
        tail = dummy;
    }

    void enq(int order_id){

        queue_node* new_node = new queue_node(order_id);

        pthread_mutex_lock(&tail_lock); 

        tail->next=new_node;
        tail=new_node;
        num_of_elements.fetch_add(1);

        pthread_mutex_unlock(&tail_lock);
    }

    Order deq(){
        Order ret;

        while (1) {
            pthread_mutex_lock(&head_lock); 

            if(head->next==nullptr){
                ret=EMPTY_QUEUE;
            }else{
                ret= head->next->id;
                head=head->next; 
                num_of_elements.fetch_sub(1);
            }
            pthread_mutex_unlock(&head_lock);

            if(ret!=EMPTY_QUEUE){
                break;
            }
        }

        return ret;

    }

};

UnderPreparationOrdersQueue* UnderPreparationOrders = new UnderPreparationOrdersQueue();

struct list_node{
    bool deleted;
    pthread_mutex_t pv_lock;
    list_node* next;
    Order order_id;

    list_node(Order id) : deleted(false), order_id(id) {pv_lock = PTHREAD_MUTEX_INITIALIZER;}
};

struct CompletedOrdersList {
    list_node* head;//centinel nodes
    list_node* tail;

    CompletedOrdersList(){
        head = new list_node(-100);
        tail = new list_node(numeric_limits<int>::max()); //infinity

        head->next=tail;
        tail->next=nullptr;

    }
};



struct District {
    CompletedOrdersList *completedOrders= new CompletedOrdersList();
    size_t checksum;

    bool validate(list_node*pred,list_node*cur);
    bool insert(Order id);
    bool deleteL(Order id);
    bool searchL(Order id);

};

bool District::validate(list_node*pred,list_node*cur){
    return !pred->deleted && !cur->deleted && pred->next == cur;
}

bool District::insert(Order id){
    list_node*pred;
    list_node*cur,*new_node;
    bool can_return=false;
    bool result=false;


    while(true){
        pred=completedOrders->head;
        cur=completedOrders->head->next;

        while(cur->order_id < id){
            pred=cur;
            cur=cur->next;
        }

        pthread_mutex_lock(&pred->pv_lock);
        pthread_mutex_lock(&cur->pv_lock);

        if (validate(pred, cur)){
            if(cur->order_id==id){
                can_return=true;
                result=false;
            }else{
            new_node = new list_node(id);
            new_node->next=cur;
            pred->next=new_node;
            can_return=true;
            result=true;
            }
        }

        pthread_mutex_unlock(&pred->pv_lock);
        pthread_mutex_unlock(&cur->pv_lock);

        if(can_return) return result;
    
    }

}

bool District::deleteL(Order id){

    list_node*pred;
    list_node*cur;
    bool can_return=false;
    bool result=false;

    while (true)
    {
        pred=completedOrders->head;
        cur=completedOrders->head->next;

        while(cur->order_id < id){
            pred=cur;
            cur=cur->next;
        }

        pthread_mutex_lock(&pred->pv_lock);
        pthread_mutex_lock(&cur->pv_lock);

        if (validate(pred, cur)){
            if(cur->order_id==id){
                cur->deleted=true;
                pred->next=cur->next;
                can_return=true;
                result=true;
            }else{
                can_return=true;
                result=false;
            }
        }

        pthread_mutex_unlock(&pred->pv_lock);
        pthread_mutex_unlock(&cur->pv_lock);

        if(can_return) return result;
    }
    
}

bool District::searchL(Order id){

    list_node*cur=completedOrders->head->next;

    while(cur->order_id < id){
        cur=cur->next;
    }

    return !cur->deleted && cur->order_id==id;
}


District Districts[DIST]; // each slot = a costumer


std::string PassStr(bool pass) {return (pass ? "PASS" : "FAIL");}

void PrintPendingOrdersEmpty(bool pass, size_t n) {
    const std::string passStr = PassStr(pass);
    printf("%s PendingOrders Empty %lu\n", passStr.data(), n);
}
void PrintUnderPreparationOrdersEmpty(bool pass, size_t n) {
    const std::string passStr = PassStr(pass);
    printf("%s UnderPreparationOrders Empty %lu\n", passStr.data(), n);
}
void PrintCompletedOrdersSize(bool pass, size_t tid, size_t n) {
    const std::string passStr = PassStr(pass);
    printf("%s District[%lu].completedOrders Size %lu\n", passStr.data(), tid, n);
}
void PrintCompletedOrdersSum(bool pass, size_t tid, size_t sum) {
    const std::string passStr = PassStr(pass);
    printf("%s District[%lu].completedOrders Sum %lu\n", passStr.data(), tid, sum);
}
void PrintCompletedOrdersValid(bool pass, size_t tid, size_t checksum) {
    const std::string passStr = PassStr(pass);
    printf("%s District[%lu].completedOrders Valid %lu\n", passStr.data(), tid, checksum);
}


void* costumer_func(void* arg){
    Order tid=*(ssize_t*) arg;
    Order order_id,i;
    Order orders_from=tid*DIST, orders_to=(tid+1)*DIST;


    for(i=orders_from; i<orders_to; i++){
        order_id= i;
        PendingOrders->push(order_id);
    }

    for(i=orders_from; i<orders_to; i++){
        while(!Districts[tid].searchL(i));
        Districts[tid].checksum+=i;
               
    }

    return nullptr;

}


void* agent_func(void* arg){
    Order order_id;

    for(Order i= 0; i<2*DIST;i++){
        order_id=PendingOrders->pop();
        UnderPreparationOrders->enq(order_id);
    }
    return nullptr;
}

Order check[DIST*DIST],counter=0;
pthread_mutex_t pr = PTHREAD_MUTEX_INITIALIZER;


void* cook_func(void* arg){
    Order order_id,tid;

    for(Order i= 0; i<2*DIST;i++){
        order_id=UnderPreparationOrders->deq();

        pthread_mutex_lock(&pr);
        check[counter]=order_id;
            counter++;
        pthread_mutex_unlock(&pr);

        tid=order_id/DIST;
        Districts[tid].insert(order_id);
    }
    return nullptr;
}


int main() {
    bool ok;
    srand(time(nullptr));

    pthread_t costumer_threads[DIST];
    pthread_t agent_threads[AGNT];
    pthread_t cook_threads[COOK];

    pthread_mutex_init(&pr, nullptr);

    for (Order i = 0; i < DIST; i++) {
        Order* arg = new Order(i);
        pthread_create(&costumer_threads[i], nullptr, costumer_func, arg);
    }

    for (Order i = 0; i < AGNT; i++) {
        Order* arg =  new Order(i);
        pthread_create(&agent_threads[i], nullptr, agent_func, arg);
    }

    for (int i = 0; i < COOK; i++) {
        Order* arg =  new Order(i);
        pthread_create(&cook_threads[i], nullptr, cook_func, arg);
    }

    
    for (Order i = 0; i < DIST; i++) {
        pthread_join(costumer_threads[i], nullptr);
    }

    for (Order i = 0; i < AGNT; i++) {
        pthread_join(agent_threads[i], nullptr);
    }

    for (Order i = 0; i < COOK; i++) {
        pthread_join(cook_threads[i], nullptr);
    }

    if(PendingOrders->top==nullptr){
        ok=1;
    }else{ ok=0;}
    PrintPendingOrdersEmpty(ok,PendingOrders->num_of_elements);

    if(UnderPreparationOrders->head->next==nullptr){
        ok=1;
    }else{ ok=0;}
    PrintUnderPreparationOrdersEmpty(ok,UnderPreparationOrders->num_of_elements);

    size_t counter=0,sum=0,checksum=0;

    for(size_t i=0; i<DIST; i++){
        counter=0;
        sum=0;
        checksum=Districts[i].checksum;
        list_node*cur=Districts[i].completedOrders->head->next;
        
        assert(cur!=NULL);

        while(cur->next!=NULL){
            counter++;
            sum+=cur->order_id;
            cur=cur->next;
        }

        printf("\n");
    
        if(counter==DIST){
            ok=true;
        }else { ok=false; }
        PrintCompletedOrdersSize(ok,i,counter);

        if(sum==i*(DIST*DIST)+(DIST-1)*DIST/2){
            ok=true;
        }else { ok=false; }
        PrintCompletedOrdersSum(ok,i,sum);

        if(checksum==sum){
            ok=true;
        }else { ok=false; }
        PrintCompletedOrdersValid(ok,i,checksum);
    } 

    // for(int i=0; i<DIST; i++){
        
    //     list_node*cur=Districts[i].completedOrders->head;
    //     printf("Client:%d ---------------------------------\n",i);
    //     while(cur->next!=NULL){
    //         printf("order: %d\n",cur->order_id);
    //         cur=cur->next;
    //     }
    // }

    // for(int i= 0; i<DIST*DIST;i++){
    //     printf("After dequeue %d\n",check[i]);
    // }

    // stack_node* cur;
    // cur=PendingOrders->top;
    // while(cur!=nullptr) {
    //     printf("inside id:%d \n",cur->tid);
    //     cur=cur->next;
    // }

    // queue_node* curr;
    // curr=UnderPreparationOrders->head->next;
    // while(curr!=nullptr) {
    //     printf("queue id:%ld \n",curr->tid);
    //     curr=curr->next;
    // }

    //printf("%d\n", N_THREADS);

    return 0;
}
