/**************************************************************** 
 **                                                            **
 **  Copyright(C) 2016 Quanergy Systems. All Rights Reserved.  **
 **  Contact: http://www.quanergy.com                          **
 **                                                            **
 ****************************************************************/

#include <quanergy/parsers/data_packet_parser_m_series.h>

namespace quanergy
{
  namespace client
  {

    DataPacketParserMSeries::DataPacketParserMSeries()
      : DataPacketParser()
      , packet_counter_(0)
      , cloud_counter_(0)
      , last_azimuth_(65000)
      , current_cloud_(new PointCloudHVDIR())
      , worker_cloud_(new PointCloudHVDIR())
      , horizontal_angle_lookup_table_(M_SERIES_NUM_ROT_ANGLES+1)
      , start_azimuth_(0)
      , degrees_per_cloud_(360.0)
    {
      // Reserve space ahead of time for incoming data
      current_cloud_->reserve(maximum_cloud_size_);
      worker_cloud_->reserve(maximum_cloud_size_);

      for (std::uint32_t i = 0; i <= M_SERIES_NUM_ROT_ANGLES; i++)
      {
        // Shift by half the rot angles to keep the number positive when wrapping.
        std::uint32_t j = (i + M_SERIES_NUM_ROT_ANGLES/2) % M_SERIES_NUM_ROT_ANGLES;

        // normalized
        double n = static_cast<double>(j) / static_cast<double>(M_SERIES_NUM_ROT_ANGLES);

        double rad = n * M_PI * 2.0 - M_PI;

        horizontal_angle_lookup_table_[i] = rad;
      }
    }

    void DataPacketParserMSeries::setReturnSelection(int return_selection)
    {
      if ((return_selection != quanergy::client::ALL_RETURNS) &&
          (return_selection < 0 || return_selection >= M_SERIES_NUM_LASERS))
      {
        throw InvalidReturnSelection();
      }

      return_selection_ = return_selection;
    }

    void DataPacketParserMSeries::setCloudSizeLimits(std::int32_t szmin, std::int32_t szmax)
    {
      if(szmin > MAX_CLOUD_SIZE || szmax > MAX_CLOUD_SIZE)
      {
        throw std::invalid_argument(std::string("Cloud size limits cannot be larger than ")
                                    + std::to_string(MAX_CLOUD_SIZE));
      }

      if(szmin > 0)
        minimum_cloud_size_ = std::max(1,szmin);
      if(szmax > 0)
        maximum_cloud_size_ = std::max(minimum_cloud_size_,szmax);
    }
    
    void DataPacketParserMSeries::setDegreesOfSweepPerCloud(double degrees_per_cloud)
    {
      if ( degrees_per_cloud < 0 || degrees_per_cloud > 360.0 ) 
      {
        throw InvalidDegreesPerCloud();
      }
      degrees_per_cloud_ = degrees_per_cloud;
    }

    void DataPacketParserMSeries::setVerticalAngles(const std::vector<double> &vertical_angles)
    {
      if (vertical_angles.size() != M_SERIES_NUM_LASERS)
      {
        throw InvalidVerticalAngles(std::string("Vertical Angles must be size: ")
                                    + std::to_string(M_SERIES_NUM_LASERS)
                                    + "; got a vector of length: "
                                    + std::to_string(vertical_angles.size()));
      }

      vertical_angle_lookup_table_.resize(vertical_angles.size());

      for (std::uint32_t i = 0; i < M_SERIES_NUM_LASERS; ++i)
      {
        vertical_angle_lookup_table_[i] = vertical_angles[i];
      }
    }

    void DataPacketParserMSeries::setVerticalAngles(SensorType sensor)
    {
      if (sensor == SensorType::M8)
      {
        std::vector<double> vertical_angles(quanergy::client::M8_VERTICAL_ANGLES,
                                            quanergy::client::M8_VERTICAL_ANGLES + quanergy::client::M_SERIES_NUM_LASERS);
        setVerticalAngles(vertical_angles);
      }
      else if (sensor == SensorType::MQ8)
      {
        std::vector<double> vertical_angles(quanergy::client::MQ8_VERTICAL_ANGLES,
                                            quanergy::client::MQ8_VERTICAL_ANGLES + quanergy::client::M_SERIES_NUM_LASERS);
        setVerticalAngles(vertical_angles);
      }
    }


    bool DataPacketParserMSeries::parse(const MSeriesDataPacket& data_packet, PointCloudHVDIRPtr& result)
    {
      // check that vertical angles have been defined
      if (vertical_angle_lookup_table_.empty())
      {
        throw InvalidVerticalAngles("In parse, the vertical angle lookup table is empty; need to call setVerticalAngles.");
      }

      bool ret = false;

      StatusType current_status = StatusType(data_packet.status);

      if (current_status != StatusType::GOOD)
      {
        if (static_cast<std::uint16_t>(data_packet.status) & static_cast<std::uint16_t>(StatusType::SENSOR_SW_FW_MISMATCH))
        {
          throw FirmwareVersionMismatchError();
        }
        else if (static_cast<std::uint16_t>(data_packet.status) & static_cast<std::uint16_t>(StatusType::WATCHDOG_VIOLATION))
        {
          throw FirmwareWatchdogViolationError();
        }

        // Status flag is set, but the value is not currently known in
        // this version of the software.  Since the status is not
        // necessarily fatal, print the status value, but otherwise do
        // nothing.
      }

      if (current_status != previous_status_)
      {
        std::cerr << "Sensor status: " << std::uint16_t(current_status) << std::endl;

        previous_status_ = current_status;
      }

      // get the timestamp of the last point in the packet as 64 bit integer in units of microseconds
      std::uint64_t current_packet_stamp ;
      if (data_packet.version <= 3 && data_packet.version != 0)
      {
        // some versions of API put 10 ns increments in this field
        current_packet_stamp = static_cast<std::uint64_t>(data_packet.seconds) * 1000000ull
                               + static_cast<std::uint64_t>(data_packet.nanoseconds) / 100ull;
      }
      else
      {
        current_packet_stamp = static_cast<std::uint64_t>(data_packet.seconds) * 1000000ull
                               + static_cast<std::uint64_t>(data_packet.nanoseconds) / 1000ull;
      }

      if (previous_packet_stamp_ == 0)
      {
        previous_packet_stamp_ = current_packet_stamp;
      }

      ++packet_counter_;

      // get spin direction
      // check 3 points in the packet to figure out which way it is spinning
      // if the measurements disagree, it could be wrap so we'll ignore that
      if (data_packet.data[0].position - data_packet.data[M_SERIES_FIRING_PER_PKT/2].position < 0
          && data_packet.data[M_SERIES_FIRING_PER_PKT/2].position - data_packet.data[M_SERIES_FIRING_PER_PKT-1].position < 0)
      {
        direction_ = 1;
      }
      else if (data_packet.data[0].position - data_packet.data[M_SERIES_FIRING_PER_PKT/2].position > 0
          && data_packet.data[M_SERIES_FIRING_PER_PKT/2].position - data_packet.data[M_SERIES_FIRING_PER_PKT-1].position > 0)
      {
        direction_ = -1;
      }

      double distance_scaling = 0.01f;
      if (data_packet.version >= 5)
      {
        distance_scaling = 0.00001f;
      }

      bool cloudfull = (current_cloud_->size() >= maximum_cloud_size_);

      // for each firing
      for (int i = 0; i < M_SERIES_FIRING_PER_PKT; ++i)
      {
        const MSeriesFiringData &data = data_packet.data[i];

        // calculate the angle in degrees
        double azimuth_angle = (static_cast<double> ((data.position+(M_SERIES_NUM_ROT_ANGLES/2))%M_SERIES_NUM_ROT_ANGLES) / (M_SERIES_NUM_ROT_ANGLES) * 360.0) - 180.;
        double delta_angle = 0;
        if ( cloud_counter_ == 0 && start_azimuth_ == 0 )
        {
          start_azimuth_ = azimuth_angle;
        } 
        else 
        {
          // calculate delta
          delta_angle = direction_*(azimuth_angle - start_azimuth_);
          while ( delta_angle < 0.0 )
          {
            delta_angle += 360.0;
          }
        }
        
        if ( delta_angle >= degrees_per_cloud_ || (degrees_per_cloud_==360.0 && (direction_*azimuth_angle < direction_*last_azimuth_) ))
        {
          start_azimuth_ = azimuth_angle;
          if (current_cloud_->size () > minimum_cloud_size_)
          {
            if(cloudfull)
            {
              std::cout << "Warning: Maximum cloud size limit of ("
                  << maximum_cloud_size_ << ") exceeded" << std::endl;
            }

						// interpolate the timestamp from the previous packet timestamp to the timestamp of this firing
            const double time_since_previous_packet =
                (current_packet_stamp - previous_packet_stamp_) * i / static_cast<double>(M_SERIES_FIRING_PER_PKT);
            const auto current_firing_stamp = static_cast<uint64_t>(std::round(
                previous_packet_stamp_ + time_since_previous_packet));

            current_cloud_->header.stamp = current_firing_stamp;
            current_cloud_->header.seq = cloud_counter_;
            current_cloud_->header.frame_id = frame_id_;

            // can't organize if we kept all points
            if (return_selection_ != quanergy::client::ALL_RETURNS)
            {
              organizeCloud(current_cloud_, worker_cloud_);
            }

            ++cloud_counter_;

            // fire the signal that we have a new cloud
            result = current_cloud_;
            ret = true;
          }
          else if(current_cloud_->size() > 0)
          {
            std::cout << "Warning: Minimum cloud size limit of (" << minimum_cloud_size_
                << ") not reached (" << current_cloud_->size() << ")" << std::endl;
          }

          // start a new cloud
          current_cloud_.reset(new PointCloudHVDIR());
          // at first we assume it is dense
          current_cloud_->is_dense = true;
          current_cloud_->reserve(maximum_cloud_size_);
          cloudfull = false;
        }

        if(cloudfull)
          continue;

        double const horizontal_angle = horizontal_angle_lookup_table_[data.position];

        for (int j = 0; j < M_SERIES_NUM_LASERS; j++)
        {
          // output point
          PointCloudHVDIR::PointType hvdir;

          double const vertical_angle = vertical_angle_lookup_table_[j];

          hvdir.h = horizontal_angle;
          hvdir.v = vertical_angle;
          hvdir.ring = j;

          if (return_selection_ == quanergy::client::ALL_RETURNS)
          {
            // for the all case, we won't keep NaN points and we'll compare
            // distances to illiminate duplicates
            // index 0 (max return) could equal index 1 (first) and/or index 2 (last)
            hvdir.intensity = data.returns_intensities[0][j];
            std::uint32_t d = data.returns_distances[0][j];
            if (d != 0)
            {
              hvdir.d = static_cast<float>(d) * distance_scaling; // convert range to meters
              // add the point to the current scan
              current_cloud_->push_back(hvdir);
            }

            if (data.returns_distances[1][j] != 0 && data.returns_distances[1][j] != d)
            {
              hvdir.intensity = data.returns_intensities[0][j];
              hvdir.d = static_cast<float>(data.returns_distances[1][j]) * distance_scaling; // convert range to meters
              // add the point to the current scan
              current_cloud_->push_back(hvdir);
            }

            if (data.returns_distances[2][j] != 0 && data.returns_distances[2][j] != d)
            {
              hvdir.intensity = data.returns_intensities[0][j];
              hvdir.d = static_cast<float>(data.returns_distances[2][j]) * distance_scaling; // convert range to meters
              // add the point to the current scan
              current_cloud_->push_back(hvdir);
            }
          }
          else
          {
            for (int i = 0; i < quanergy::client::M_SERIES_NUM_LASERS; ++i)
            {
              if (return_selection_ == i)
              {
                hvdir.intensity = data.returns_intensities[i][j];

                if (data.returns_distances[i][j] == 0)
                {
                  hvdir.d = std::numeric_limits<float>::quiet_NaN();
                  // if the range is NaN, the cloud is not dense
                  current_cloud_->is_dense = false;
                }
                else
                {
                  hvdir.d = static_cast<float>(data.returns_distances[i][j]) * distance_scaling; // convert range to meters
                }

                // add the point to the current scan
                current_cloud_->push_back(hvdir);
              }
            }
          }
        }

        last_azimuth_ = azimuth_angle;
      }

      previous_packet_stamp_ = current_packet_stamp;

      return ret;
    }

    void DataPacketParserMSeries::organizeCloud(PointCloudHVDIRPtr & current_pc,
                                           PointCloudHVDIRPtr & temp_pc)
    {
      // transpose the cloud
      temp_pc->clear();

      temp_pc->header.stamp = current_pc->header.stamp;
      temp_pc->header.seq = current_pc->header.seq;
      temp_pc->header.frame_id = current_pc->header.frame_id;

      // reserve space
      temp_pc->reserve(current_pc->size());

      unsigned int temp_index;
      unsigned int width = current_pc->size () / M_SERIES_NUM_LASERS; // CONSTANT FOR NUM BEAMS

      // iterate through each ring from top down
      for (int i = M_SERIES_NUM_LASERS - 1; i >= 0; --i)
      {
        // iterate through width in collect order
        for (unsigned int j = 0; j < width; ++j)
        {
          // original data is in collect order and laser order
          temp_index = j * M_SERIES_NUM_LASERS + i;

          temp_pc->push_back(current_pc->points[temp_index]);
        }
      }

      current_pc.swap(temp_pc);

      current_pc->height = M_SERIES_NUM_LASERS;
      current_pc->width  = width;
    }

  } // namespace client

} // namespace quanergy
