#ifndef _TiBANK_ALIVE_TIMER_H__
#define _TiBANK_ALIVE_TIMER_H__

#include <set>

#include <boost/unordered_map.hpp> // C++11

#include "Log.h"

template<typename T>
class AliveItem {
public:
	AliveItem(time_t tm, std::shared_ptr<T> t_ptr):
		time_(tm), raw_ptr_(t_ptr.get()), weak_ptr_(t_ptr) {
	}

	time_t get_expire_time() {
		return time_;
	}

    T* get_raw_ptr() {
        return raw_ptr_;
    }

    std::weak_ptr<T> get_weak_ptr() {
        return weak_ptr_;
    }

private:
	time_t time_;
    T*     raw_ptr_;
	std::weak_ptr<T> weak_ptr_;
};

template<typename T>
class AliveTimer {
public:
    typedef std::shared_ptr<AliveItem<T> >               active_item_ptr;
    typedef std::map<time_t, std::set<active_item_ptr> > TimeContainer;       //
    typedef std::map<T*, active_item_ptr >               BucketContainer;     //
    typedef boost::function<int(std::shared_ptr<T>)>     ExpiredHandler;

public:

    explicit AliveTimer(std::string alive_name, time_t time_out = 10*60, time_t time_linger = 30):
		alive_name_(alive_name),
        lock_(), time_items_(), bucket_items_(), func_(),
        time_out_(time_out), time_linger_(time_linger) {
        initialized_ = false;
	 }

	 explicit AliveTimer(std::string alive_name, ExpiredHandler func, time_t time_out = 10*60, time_t time_linger = 30):
		alive_name_(alive_name),
        lock_(), time_items_(), bucket_items_(), func_(func),
        time_out_(time_out), time_linger_(time_linger) {

        initialized_ = true;
	 }

    bool init(ExpiredHandler func) {
		if (initialized_) {
			log_err("This AliveTimer %s already initialized, please check!", alive_name_.c_str());
			return true;
		}

		func_ = func;
        initialized_ = true;

        return true;
    }

	bool init(ExpiredHandler func, time_t time_out, time_t time_linger) {
		if (initialized_) {
			log_err("This AliveTimer %s already initialized, please check!", alive_name_.c_str());
			return true;
		}

		func_ = func;
		time_out_ = time_out;
		time_linger_ = time_linger;
        initialized_ = true;

        return true;
    }

	 ~AliveTimer(){}

    bool touch(std::shared_ptr<T> ptr) {
        time_t tm = ::time(NULL) + time_out_;
        return touch(ptr, tm);
	 }

    bool touch(std::shared_ptr<T> ptr, time_t tm) {
        boost::unique_lock<boost::mutex> lock(lock_);
        typename BucketContainer::iterator iter = bucket_items_.find(ptr.get());
        if (iter == bucket_items_.end()) {
            log_err("touched item not found!");
            return false;
        }

        time_t before = iter->second->get_expire_time();
        if (tm - before < time_linger_){
            log_debug("Linger optimize: %ld, %ld", tm, before);
            return true;
        }

        if (time_items_.find(before) == time_items_.end()){
            safe_assert(false);
            log_err("bucket tm: %ld not found, critical error!", before);
            return false;
        }

        if (time_items_.find(tm) == time_items_.end()) {
            time_items_[tm] = std::set<active_item_ptr>();  // create new time bucket
        }

        time_items_[tm].insert(iter->second);
        time_items_[before].erase(iter->second);
        log_debug("touched: %p, %ld -> %ld", ptr.get(), before, tm);
        return true;
	}

    bool insert(std::shared_ptr<T> ptr) {
        time_t tm = ::time(NULL) + time_out_;
        return insert(ptr, tm);
    }

    bool insert(std::shared_ptr<T> ptr, time_t tm ) {
        boost::unique_lock<boost::mutex> lock(lock_);
        typename BucketContainer::iterator iter = bucket_items_.find(ptr.get());
        if (iter != bucket_items_.end()) {
		log_err("Insert item already exists: @ %ld, %p", iter->second->get_expire_time(),
                           iter->second->get_raw_ptr());
		return false;
        }

        active_item_ptr alive_item = std::make_shared<AliveItem<T> >(tm, ptr);
        if (!alive_item){
            log_err("Create AliveItem failed!");
            return false;
        }

        bucket_items_[ptr.get()] = alive_item;

        if (time_items_.find(tm) == time_items_.end()) {
            time_items_[tm] = std::set<active_item_ptr>();
        }
        time_items_[tm].insert(alive_item);

        log_debug("inserted: %p, %ld", ptr.get(), tm);
        return true;
    }

    bool clean_up() {

        if (!initialized_) {
            log_err("not initialized, please check ...");
            return false;
        }

        boost::unique_lock<boost::mutex> lock(lock_);

		struct timeval checked_start;
		::gettimeofday(&checked_start, NULL);
		int checked_count = 0;
        time_t current_sec = ::time(NULL);

        typename TimeContainer::iterator iter = time_items_.begin();
        typename TimeContainer::iterator remove_iter = time_items_.end();
        for ( ; iter != time_items_.end(); ){
            if (iter->first < current_sec) {
                typename std::set<active_item_ptr>::iterator it = iter->second.begin();
                for (; it != iter->second.end(); ++it) {
                    T* p = (*it)->get_raw_ptr();
                    if (bucket_items_.find(p) == bucket_items_.end()) {
                        safe_assert(false);
                        log_err("bucket item: %p not found, critical error!", p);
                    }

                    log_debug("bucket item remove: %p, %ld", p, (*it)->get_expire_time());
                    bucket_items_.erase(p);
                    std::weak_ptr<T> weak_item = (*it)->get_weak_ptr();
                    if (std::shared_ptr<T> ptr = weak_item.lock()) {
                        func_(ptr);
                    } else {
                        log_err("item %p may already release before ...", p);
                    }
                }

                // (Old style) References and iterators to the erased elements are invalidated.
                // Other references and iterators are not affected.

                log_debug("expire entry remove: %ld, now:%ld count:%ld", iter->first, current_sec, iter->second.size());
                remove_iter = iter ++;
                time_items_.erase(remove_iter);
            }
            else {
				       // time_t 是已经排序了的
                break; // all expired clean
            }

			++ checked_count;
			if ((checked_count % 10) == 0) {  // 不能卡顿太长时间，否则正常的请求会被卡死
				struct timeval checked_now;
				::gettimeofday(&checked_now, NULL);
				int64_t elapse_ms = ( 1000000 * ( checked_now.tv_sec - checked_start.tv_sec ) + checked_now.tv_usec - checked_start.tv_usec ) / 1000;
				if (elapse_ms > 15) {
					log_notice("check works too long elapse time: %ld ms, break now", elapse_ms);
					break;
				}
			}
        }

        int64_t total_count = 0;
        for (iter = time_items_.begin() ; iter != time_items_.end(); ++ iter) {
            total_count += iter->second.size();
        }
        log_debug("current alived hashed count:%ld, timed_count: %ld", bucket_items_.size(), total_count);
    }

private:
    bool initialized_;
	std::string     alive_name_;
    mutable boost::mutex lock_;
    TimeContainer   time_items_;
    BucketContainer bucket_items_;
    ExpiredHandler  func_;

    time_t time_out_;
    time_t time_linger_;
};


#endif // _TiBANK_ALIVE_TIMER_H__

