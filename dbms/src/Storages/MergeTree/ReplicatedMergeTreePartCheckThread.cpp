#include <Storages/MergeTree/ReplicatedMergeTreePartCheckThread.h>
#include <Storages/MergeTree/checkDataPart.h>
#include <Storages/StorageReplicatedMergeTree.h>
#include <Common/setThreadName.h>


namespace ProfileEvents
{
    extern const Event ReplicatedPartChecks;
    extern const Event ReplicatedPartChecksFailed;
    extern const Event ReplicatedDataLoss;
}

namespace DB
{

static const auto PART_CHECK_ERROR_SLEEP_MS = 5 * 1000;


ReplicatedMergeTreePartCheckThread::ReplicatedMergeTreePartCheckThread(StorageReplicatedMergeTree & storage_)
    : storage(storage_),
    log(&Logger::get(storage.database_name + "." + storage.table_name + " (StorageReplicatedMergeTree, PartCheckThread)"))
{
}


void ReplicatedMergeTreePartCheckThread::start()
{
    std::lock_guard<std::mutex> lock(start_stop_mutex);

    if (need_stop)
        need_stop = false;
    else
        thread = std::thread([this] { run(); });
}


void ReplicatedMergeTreePartCheckThread::stop()
{
    std::lock_guard<std::mutex> lock(start_stop_mutex);

    need_stop = true;
    if (thread.joinable())
    {
        wakeup_event.set();
        thread.join();
        need_stop = false;
    }
}


void ReplicatedMergeTreePartCheckThread::enqueuePart(const String & name, time_t delay_to_check_seconds)
{
    std::lock_guard<std::mutex> lock(parts_mutex);

    if (parts_set.count(name))
        return;

    parts_queue.emplace_back(name, time(nullptr) + delay_to_check_seconds);
    parts_set.insert(name);
    wakeup_event.set();
}


size_t ReplicatedMergeTreePartCheckThread::size() const
{
    std::lock_guard<std::mutex> lock(parts_mutex);
    return parts_set.size();
}


void ReplicatedMergeTreePartCheckThread::searchForMissingPart(const String & part_name)
{
    auto zookeeper = storage.getZooKeeper();
    String part_path = storage.replica_path + "/parts/" + part_name;

    /// If the part is in ZooKeeper, remove it from there and add the task to download it to the queue.
    if (zookeeper->exists(part_path))
    {
        LOG_WARNING(log, "Part " << part_name << " exists in ZooKeeper but not locally. "
            "Removing from ZooKeeper and queueing a fetch.");
        ProfileEvents::increment(ProfileEvents::ReplicatedPartChecksFailed);

        storage.removePartAndEnqueueFetch(part_name);
        return;
    }

    /// If the part is not in ZooKeeper, we'll check if it's at least somewhere.
    auto part_info = MergeTreePartInfo::fromPartName(part_name, storage.data.format_version);

    /** The logic is as follows:
        * - if some live or inactive replica has such a part, or a part covering it
        *   - it is Ok, nothing is needed, it is then downloaded when processing the queue, when the replica comes to life;
        *   - or, if the replica never comes to life, then the administrator will delete or create a new replica with the same address and see everything from the beginning;
        * - if no one has such part or a part covering it, then
        *   - if there are two smaller parts, one with the same min block and the other with the same
        *     max block, we hope that all parts in between are present too and the needed part
        *     will appear on other replicas as a result of a merge.
        *   - otherwise, consider the part lost and delete the entry from the queue.
        *
        *   Note that this logic is not perfect - some part in the interior may be missing and the
        *   needed part will never appear. But precisely determining whether the part will appear as
        *   a result of a merge is complicated - we can't just check if all block numbers covered
        *   by the missing part are present somewhere (because gaps between blocks are possible)
        *   and to determine the constituent parts of the merge we need to query the replication log
        *   (both the common log and the queues of the individual replicas) and then, if the
        *   constituent parts are in turn not found, solve the problem recursively for them.
        *
        *   Considering the part lost when it is not in fact lost is very dangerous because it leads
        *   to divergent replicas and intersecting parts. So we err on the side of caution
        *   and don't delete the queue entry when in doubt.
        */

    LOG_WARNING(log, "Checking if anyone has a part covering " << part_name << ".");

    bool found_part_with_the_same_min_block = false;
    bool found_part_with_the_same_max_block = false;

    Strings replicas = zookeeper->getChildren(storage.zookeeper_path + "/replicas");
    for (const String & replica : replicas)
    {
        Strings parts = zookeeper->getChildren(storage.zookeeper_path + "/replicas/" + replica + "/parts");
        for (const String & part_on_replica : parts)
        {
            auto part_on_replica_info = MergeTreePartInfo::fromPartName(part_on_replica, storage.data.format_version);

            if (part_on_replica_info.contains(part_info))
            {
                LOG_WARNING(log, "Found part " << part_on_replica << " on " << replica << " that covers the missing part " << part_name);
                return;
            }

            if (part_info.contains(part_on_replica_info))
            {
                if (part_on_replica_info.min_block == part_info.min_block)
                    found_part_with_the_same_min_block = true;
                if (part_on_replica_info.max_block == part_info.max_block)
                    found_part_with_the_same_max_block = true;

                if (found_part_with_the_same_min_block && found_part_with_the_same_max_block)
                {
                    LOG_WARNING(log,
                        "Found parts with the same min block and with the same max block as the missing part "
                        << part_name << ". Hoping that it will eventually appear as a result of a merge.");
                    return;
                }
            }
        }
    }

    /// No one has such a part and the merge is impossible.
    String not_found_msg;
    if (found_part_with_the_same_min_block)
        not_found_msg = "a smaller part with the same max block.";
    else if (found_part_with_the_same_min_block)
        not_found_msg = "a smaller part with the same min block.";
    else
        not_found_msg = "smaller parts with either the same min block or the same max block.";
    LOG_ERROR(log, "No replica has part covering " << part_name
              << " and a merge is impossible: we didn't find " << not_found_msg);

    ProfileEvents::increment(ProfileEvents::ReplicatedPartChecksFailed);

    /// Is it in the replication queue? If there is - delete, because the task can not be processed.
    if (!storage.queue.remove(zookeeper, part_name))
    {
        /// The part was not in our queue. Why did it happen?
        LOG_ERROR(log, "Missing part " << part_name << " is not in our queue.");
        return;
    }

    /** This situation is possible if on all the replicas where the part was, it deteriorated.
        * For example, a replica that has just written it has power turned off and the data has not been written from cache to disk.
        */
    LOG_ERROR(log, "Part " << part_name << " is lost forever.");
    ProfileEvents::increment(ProfileEvents::ReplicatedDataLoss);

    /** You need to add the missing part to `block_numbers` so that it does not interfere with merges.
      * But we can't just add it to `block_numbers` - if so,
      *  ZooKeeper for some reason will skip one number for autoincrement,
      *  and there will still be a hole in the block numbers.
      * Especially because of this, you have to separately have `nonincrement_block_numbers`.
      *
      * By the way, if we die here, the mergers will not be made through these missing parts.
      *
      * And, we will not add if:
      * - would need to create too many (more than 1000) nodes;
      * - the part is the first in partition or was ATTACHed.
      * NOTE It is possible to also add a condition if the entry in the queue is very old.
      */

    size_t part_length_in_blocks = part_info.max_block + 1 - part_info.min_block;
    if (part_length_in_blocks > 1000)
    {
        LOG_ERROR(log, "Won't add nonincrement_block_numbers because part spans too many blocks (" << part_length_in_blocks << ")");
        return;
    }

    const String & partition_id = part_info.partition_id;
    for (auto i = part_info.min_block; i <= part_info.max_block; ++i)
    {
        zookeeper->createIfNotExists(storage.zookeeper_path + "/nonincrement_block_numbers/" + partition_id, "");
        AbandonableLockInZooKeeper::createAbandonedIfNotExists(
            storage.zookeeper_path + "/nonincrement_block_numbers/" + partition_id + "/block-" + padIndex(i),
            *zookeeper);
    }
}


void ReplicatedMergeTreePartCheckThread::checkPart(const String & part_name)
{
    LOG_WARNING(log, "Checking part " << part_name);
    ProfileEvents::increment(ProfileEvents::ReplicatedPartChecks);

    /// If the part is still in the PreCommitted -> Committed transition, it is not lost
    /// and there is no need to go searching for it on other replicas. To definitely find the needed part
    /// if it exists (or a part containing it) we first search among the PreCommitted parts.
    auto part = storage.data.getPartIfExists(part_name, {MergeTreeDataPartState::PreCommitted});
    if (!part)
        part = storage.data.getActiveContainingPart(part_name);

    /// We do not have this or a covering part.
    if (!part)
    {
        searchForMissingPart(part_name);
    }
    /// We have this part, and it's active. We will check whether we need this part and whether it has the right data.
    else if (part->name == part_name)
    {
        auto zookeeper = storage.getZooKeeper();
        auto table_lock = storage.lockStructure(false, __PRETTY_FUNCTION__);

        /// If the part is in ZooKeeper, check its data with its checksums, and them with ZooKeeper.
        if (zookeeper->exists(storage.replica_path + "/parts/" + part_name))
        {
            LOG_WARNING(log, "Checking data of part " << part_name << ".");

            try
            {
                auto zk_checksums = MergeTreeData::DataPart::Checksums::parse(
                    zookeeper->get(storage.replica_path + "/parts/" + part_name + "/checksums"));
                zk_checksums.checkEqual(part->checksums, true);

                auto zk_columns = NamesAndTypesList::parse(
                    zookeeper->get(storage.replica_path + "/parts/" + part_name + "/columns"));
                if (part->columns != zk_columns)
                    throw Exception("Columns of local part " + part_name + " are different from ZooKeeper");

                checkDataPart(
                    storage.data.getFullPath() + part_name,
                    storage.data.index_granularity,
                    true,
                    storage.data.primary_key_data_types,
                    [this] { return need_stop.load(); });

                if (need_stop)
                {
                    LOG_INFO(log, "Checking part was cancelled.");
                    return;
                }

                LOG_INFO(log, "Part " << part_name << " looks good.");
            }
            catch (const Exception & e)
            {
                /// TODO Better to check error code.

                tryLogCurrentException(__PRETTY_FUNCTION__);

                LOG_ERROR(log, "Part " << part_name << " looks broken. Removing it and queueing a fetch.");
                ProfileEvents::increment(ProfileEvents::ReplicatedPartChecksFailed);

                storage.removePartAndEnqueueFetch(part_name);

                /// Delete part locally.
                storage.data.renameAndDetachPart(part, "broken_");
            }
        }
        else if (part->modification_time + MAX_AGE_OF_LOCAL_PART_THAT_WASNT_ADDED_TO_ZOOKEEPER < time(nullptr))
        {
            /// If the part is not in ZooKeeper, delete it locally.
            /// Probably, someone just wrote down the part, and has not yet added to ZK.
            /// Therefore, delete only if the part is old (not very reliable).
            ProfileEvents::increment(ProfileEvents::ReplicatedPartChecksFailed);

            LOG_ERROR(log, "Unexpected part " << part_name << " in filesystem. Removing.");
            storage.data.renameAndDetachPart(part, "unexpected_");
        }
        else
        {
            /// TODO You need to make sure that the part is still checked after a while.
            /// Otherwise, it's possible that the part was not added to ZK,
            ///  but remained in the filesystem and in a number of active parts.
            /// And then for a long time (before restarting), the data on the replicas will be different.

            LOG_TRACE(log, "Young part " << part_name
                << " with age " << (time(nullptr) - part->modification_time)
                << " seconds hasn't been added to ZooKeeper yet. It's ok.");
        }
    }
    else
    {
        /// If we have a covering part, ignore all the problems with this part.
        /// In the worst case, errors will still appear `old_parts_lifetime` seconds in error log until the part is removed as the old one.
        LOG_WARNING(log, "We have part " << part->name << " covering part " << part_name);
    }
}


void ReplicatedMergeTreePartCheckThread::run()
{
    setThreadName("ReplMTPartCheck");

    while (!need_stop)
    {
        try
        {
            time_t current_time = time(nullptr);

            /// Take part from the queue for verification.
            PartsToCheckQueue::iterator selected = parts_queue.end();    /// end from std::list is not get invalidated
            time_t min_check_time = std::numeric_limits<time_t>::max();

            {
                std::lock_guard<std::mutex> lock(parts_mutex);

                if (parts_queue.empty())
                {
                    if (!parts_set.empty())
                    {
                        LOG_ERROR(log, "Non-empty parts_set with empty parts_queue. This is a bug.");
                        parts_set.clear();
                    }
                }
                else
                {
                    for (auto it = parts_queue.begin(); it != parts_queue.end(); ++it)
                    {
                        if (it->second <= current_time)
                        {
                            selected = it;
                            break;
                        }

                        if (it->second < min_check_time)
                            min_check_time = it->second;
                    }
                }
            }

            if (selected == parts_queue.end())
            {
                /// Poco::Event is triggered immediately if `signal` was before the `wait` call.
                /// We can wait a little more than we need due to the use of the old `current_time`.

                if (min_check_time != std::numeric_limits<time_t>::max() && min_check_time > current_time)
                    wakeup_event.tryWait(1000 * (min_check_time - current_time));
                else
                    wakeup_event.wait();

                continue;
            }

            checkPart(selected->first);

            if (need_stop)
                break;

            /// Remove the part from check queue.
            {
                std::lock_guard<std::mutex> lock(parts_mutex);

                if (parts_queue.empty())
                {
                    LOG_ERROR(log, "Someone erased cheking part from parts_queue. This is a bug.");
                }
                else
                {
                    parts_set.erase(selected->first);
                    parts_queue.erase(selected);
                }
            }
        }
        catch (...)
        {
            tryLogCurrentException(__PRETTY_FUNCTION__);
            wakeup_event.tryWait(PART_CHECK_ERROR_SLEEP_MS);
        }
    }

    LOG_DEBUG(log, "Part check thread finished");
}

}
