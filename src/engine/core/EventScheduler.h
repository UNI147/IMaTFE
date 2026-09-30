#pragma once
#include "Types.h"
#include <functional>
#include <map>
namespace imatfe::core {
class EventScheduler { public: using Cycle=u64; using Id=u64; Id schedule_at(Cycle t,std::function<void()> cb){const Id id=++next_; events_.emplace(std::make_pair(t,id),Entry{id,std::move(cb),false}); return id;} Id schedule_after(Cycle d,std::function<void()> cb){return schedule_at(now_+d,std::move(cb));} bool cancel(Id id){for(auto& [k,e]:events_)if(e.id==id){e.cancelled=true;return true;}return false;} void run_until(Cycle t){if(t<now_) return; while(!events_.empty()){auto it=events_.begin();if(it->first.first>t)break;const Cycle event_cycle=it->first.first;auto e=std::move(it->second);events_.erase(it);now_=event_cycle;if(!e.cancelled&&e.cb)e.cb();}now_=t;} void reset(){events_.clear();now_=0;next_=0;} Cycle now()const noexcept{return now_;} private: struct Entry{Id id;std::function<void()> cb;bool cancelled;}; std::map<std::pair<Cycle,Id>,Entry> events_; Cycle now_=0; Id next_=0;};
}
