#include "hashmap.h"
//#include <stdio.h>
//#include <stdlib.h>
#include <linux/string.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>


int default_hash(void * elem){
	int * ptr = (int *) elem;
	return *ptr;
}

int default_key_comparer(void * key1, void * key2){
	

	if(key1 == key2)
		return 0;
	else
		return 1;
}


int int_key_comparer(int * key1, int * key2){
	
	if(key1 == NULL || key2 == NULL) {
		if(key1 == NULL && key2 != NULL) {
			printk(KERN_INFO "LLIST Key1 NULL, Key2 = %d\n", *key2);
		}
		
		if(key2 == NULL && key1 != NULL) {
			printk(KERN_INFO "LLIST Key2 NULL, Key1 = %d\n", *key1);
		}
	
		return 1;
	}
	
	if(*key1 == *key2)
		return 0;
	else
		return 1;
	
}

int str_key_comparer(char * key1, char * key2){

	return strcmp(key1,key2);
}



int str_hash(char * s)
{

    //http://stackoverflow.com/questions/114085/fast-string-hashing-algorithm-with-low-collision-rates-with-32-bit-integer
    int hash = 0;
    char * ptr = s;
    int i = 0;
    while(*ptr != NULL)
    {
    	hash += *ptr;
    	hash += (hash << 10);
    	hash ^= (hash >> 6);

        ++ptr;
    }

    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);


    return hash;
}

int int_hash(int * val){

	char buffer[33];
	int hash;
	memset(buffer,0,33);
	sprintf(buffer,"%d", *val);
	hash = str_hash(buffer);
	return hash;

}

int hmap_elem_comparer(hashmap_elem * elem1, hashmap_elem * elem2){

	if(elem1 != NULL && elem2 != NULL) {
		return elem1->equals(elem1->key, elem2->key);
	}
	else {
		return 1;
	}

}

void hmap_init(hashmap * h, char * type , int m_size){
	
	int i = 0;
	int map_size;

	if(m_size == 0)
		map_size = DEFAULT_MAP_SIZE;
	else
		map_size = m_size;
	
	h->size = map_size;
	h->head = kmalloc(sizeof(llist *) * map_size, GFP_KERNEL);
	for(i = 0; i < map_size; i++){
		h->head[i] = (llist *) kmalloc(sizeof(llist), GFP_KERNEL);
		llist_init(h->head[i]);
	}



	for(i = 0; i < h->size; i++){
		llist_set_equality_checker(h->head[i],hmap_elem_comparer);
	}
	
	if(strcmp(type,"int") == 0){
		
		h->hash = int_hash;
		h->key_comparer = int_key_comparer;
	}
	else{		
		if(strcmp(type,"string") == 0){
			h->hash = str_hash;
			h->key_comparer = str_key_comparer;
		}
		else{
			h->hash = default_hash;
			h->key_comparer = default_key_comparer;
		}
	}

}

void hmap_set_hash(hashmap * h, int (*hashfn) (void * item)){
	h->hash = hashfn;
}

void hmap_set_comparer(hashmap * h, int (*comparerfn) (void * elem1, void * elem2)){
	h->key_comparer = comparerfn;
}

void hmap_put(hashmap * h, void * key, void * value){
	int index;
	hashmap_elem * new_elem;
	hashmap_elem * temp;

	if(h == NULL || key == NULL)
		return;

	
	
	index = (abs(h->hash(key)) % h->size);
	
	llist * list;	
	list = h->head[index];
	llist_elem * head;

	new_elem = (hashmap_elem *) kmalloc(sizeof(hashmap_elem), GFP_KERNEL);
	memset(new_elem,0,sizeof(hashmap_elem));
	new_elem->key = key;
	new_elem->value = value;
	new_elem->equals = h->key_comparer;
	head = list->head;
	while(head != NULL){
		if(list->equals(head->item,new_elem) == 0){
			temp = (hashmap_elem *) head->item;
			temp->key = key;
			temp->value = value;
			kfree(new_elem);
			return;
		}
		head = head->next;
	}

	llist_append(list,new_elem);
	

	

}

void hmap_put_abs(hashmap * h, int key, void * value){
	int index;
	hashmap_elem * new_elem;
	hashmap_elem * temp;

	if(h == NULL)
		return;

	
	
	index = key % h->size;
	llist * list;	
	list = h->head[index];
	llist_elem * head;

	
	head = list->head;
	while(head != NULL){
		temp = (hashmap_elem *) head->item;
		if(temp->key_val == key) {
			printk(KERN_INFO "HMAP: Updating existing value. Key: %d\n", key);
			temp->value = value;
			return;
		}
		head = head->next;
	}

	new_elem = (hashmap_elem *) kmalloc(sizeof(hashmap_elem), GFP_KERNEL);

	if(new_elem){
		//new_elem->key = NULL;
		new_elem->key_val = key;
		new_elem->value = value;
		new_elem->equals = NULL;

		llist_append(list,new_elem);

	}

}

void* hmap_get(hashmap * h, void * key){
	
	int index;
	hashmap_elem * new_elem;	
	hashmap_elem * temp;

	if(h == NULL || key == NULL)
		return NULL;

	index = (abs(h->hash(key)) % h->size);
	
	llist * list;	
	list = h->head[index];
	llist_elem * head = list->head;

	new_elem = (hashmap_elem *) kmalloc(sizeof(hashmap_elem), GFP_KERNEL);
	new_elem->key = key;
	new_elem->value = NULL;
	new_elem->equals = h->key_comparer;

	while(head != NULL){
		if(list->equals(head->item,new_elem) == 0){
			kfree(new_elem);
			temp = (hashmap_elem *) head->item;
			
			if(temp->value == NULL)
				printk(KERN_INFO "HMAP: Value exists but is NULL\n");
			return temp->value;
		}
		head = head->next;
	}
	kfree(new_elem);
	return NULL;

}

void* hmap_get_abs(hashmap * h, int key){
	
	int index;
	hashmap_elem * new_elem;	
	hashmap_elem * temp;

	if(h == NULL)
		return NULL;

	index = (key) % h->size;
	
	llist * list;	
	list = h->head[index];
	llist_elem * head = list->head;

	while(head != NULL){
		temp = (hashmap_elem *) head->item;
		if(temp == NULL){
			head = head->next;
			printk(KERN_INFO "HMAP: Item is NULL. Key = %d\n", key);
			continue;
		}
		if(temp->key_val ==key) {
			if(temp->value == NULL)
				printk(KERN_INFO "HMAP: Value exists but is NULL\n");
			return temp->value;
		}
		head = head->next;
	}

	return NULL;

}


void hmap_remove(hashmap * h, void * key){

	int index,i;
	hashmap_elem * new_elem;
	hashmap_elem * temp;
		
	index = (abs(h->hash(key)) % h->size);
	llist * list;	
	list = h->head[index];
	llist_elem * head = list->head;

	new_elem = (hashmap_elem *) kmalloc(sizeof(hashmap_elem), GFP_KERNEL);
	new_elem->key = key;
	new_elem->value = NULL;
	new_elem->equals = h->key_comparer;
	i = 0;
	while(head != NULL){
		if(list->equals(head->item,new_elem) == 0){
			temp = llist_remove_at(list,i);
			kfree(temp);
			kfree(new_elem);
			return ;
		}
		i++;
		head = head->next;
	}
	kfree(new_elem);

}

void hmap_remove_abs(hashmap * h, int key){

	int index,i;
	hashmap_elem * new_elem;
	hashmap_elem * temp;
		
	index = (key) % h->size;
	llist * list;	
	list = h->head[index];
	llist_elem * head = list->head;
	
	if(h == NULL)
		return;

	i = 0;
	while(head != NULL){
		temp = (hashmap_elem *) head->item;
		if(temp->key_val == key) {
			//llist_remove_at(list,i);
			//kfree(temp);
			temp->key_val = 0;
			return;			
		}
		i++;
		head = head->next;
	}
}



void hmap_destroy(hashmap * h){

	int i;
	llist * l;
	llist_elem * head;
	llist_elem * temp;
	for(i = 0; i < h->size; i++){
		l = h->head[i];
		head = l->head;
		while(head != NULL){
			temp = head;
			head = head->next;
			kfree(temp->item);
			kfree(temp);
		}
		kfree(h->head[i]);
	}
	kfree(h->head);
	

}


