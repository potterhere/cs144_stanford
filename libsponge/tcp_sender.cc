#include "tcp_sender.hh"

#include "tcp_config.hh"

#include <random>
#include <iostream>

// Dummy implementation of a TCP sender

// For Lab 3, please replace with a real implementation that passes the
// automated checks run by `make check_lab3`.

template <typename... Targs>
void DUMMY_CODE(Targs &&... /* unused */) {}

using namespace std;

//! \param[in] capacity the capacity of the outgoing byte stream
//! \param[in] retx_timeout the initial amount of time to wait before retransmitting the oldest outstanding segment
//! \param[in] fixed_isn the Initial Sequence Number to use, if set (otherwise uses a random ISN)
TCPSender::TCPSender(const size_t capacity, const uint16_t retx_timeout, const std::optional<WrappingInt32> fixed_isn)
    : _isn(fixed_isn.value_or(WrappingInt32{random_device()()}))
    , _initial_retransmission_timeout{retx_timeout}
    , _stream(capacity)
	, _rto(retx_timeout)
	, _retransmissionTimer(0)
	, _byte_in_flight(0)
	{}
	

uint64_t TCPSender::bytes_in_flight() const { return {_byte_in_flight}; }

void TCPSender::fill_window() {
	if (_fin_sent)
		return;
		
	if (_next_seqno == 0)
	{
		TCPSegment tcpSegment;
		// sync pack
		_syn_sent = true;
		tcpSegment.header().syn = true;

		send_non_empty_segment(tcpSegment);
		return;		
	}

	size_t max_payload_size = TCPConfig::MAX_PAYLOAD_SIZE;	
	while (_notifyWinSize) // can send
	{
		//if (_stream.eof() && tcpSegment.length_in_sequence_space() < _notifyWinSize)
		if (_stream.eof() && !_fin_sent)
		{
			TCPSegment tcpSegment;	
			tcpSegment.header().fin = true;
			_fin_sent = true;
			send_non_empty_segment(tcpSegment);
			return;
		} else if (_stream.eof()) {
			return;
		}
		else{
			// read ad many as possible, said by Manul if fill_window
			uint16_t n_read_current = min(_notifyWinSize, static_cast<uint16_t>(max_payload_size));
			Buffer buffer(std::move(_stream.read(n_read_current)));
			// cerr << "read from _stream:" << buffer.copy() << endl;

			TCPSegment tcpSegment;
			tcpSegment.payload() = buffer;
			if (_stream.input_ended() && tcpSegment.length_in_sequence_space() < _notifyWinSize)
			{
				tcpSegment.header().fin = true;
				_fin_sent = true;
			}

			if (tcpSegment.length_in_sequence_space() == 0)
				return;

			send_non_empty_segment(tcpSegment);
			_notifyWinSize -= tcpSegment.length_in_sequence_space();
		}
	}
}

//! \param ackno The remote receiver's ackno (acknowledgment number)
//! \param window_size The remote receiver's advertised window size
//! \returns `false` if the ackno appears invalid (acknowledges something the TCPSender hasn't sent yet)
bool TCPSender::ack_received(const WrappingInt32 ackno, const uint16_t window_size) {
    uint64_t abs_seqno = unwrap(ackno, _isn, _next_seqno);
	
	// sent with more
	if (abs_seqno > _next_seqno)
		return false;
	
	// cerr << "abs_seqno:" << abs_seqno 
		 // << " ackno:" << ackno.raw_value() 
		 // << " next_seqno:" << _next_seqno << endl;
	
	_notifyWinSize = window_size;
	
	// query from the small seqno
	auto it = mapOutstandingSegment.begin();
	vector<decltype(it)> toDelete;
	
	for (; it != mapOutstandingSegment.end(); ++it)
	{
		uint64_t seqno_ = it->first;
		size_t segment_size = it->second.tcpSegment.length_in_sequence_space();
		
		// cerr << "   seqno_:" << seqno_ 
			 // << " segment_size:" << segment_size 
			 // << " abs_seqno:" << abs_seqno 
			 // << " byteInFlight:" << _byte_in_flight << endl;
		if ((seqno_ + segment_size) <= abs_seqno)
		{			
			_byte_in_flight -= segment_size;

			toDelete.push_back(it);
		}
		else
		{
			break;
		}
	}
	// cerr << "  new byteInFlight:" << _byte_in_flight << endl;
	
	for (auto i: toDelete)
        mapOutstandingSegment.erase(i);
	
	if (!mapOutstandingSegment.empty())
        _retransmissionTimer = 0;
	
	_consecutive_retransmission = 0;
	
	_rto = _initial_retransmission_timeout;

	// fill next
	fill_window();
	
    return true;
}

//! \param[in] ms_since_last_tick the number of milliseconds since the last call to this method
void TCPSender::tick(const size_t ms_since_last_tick) {	
	// update total
	_alive_time += ms_since_last_tick;
	
	// check earliest segment if has!!
	if (not mapOutstandingSegment.empty())
		_retransmissionTimer += ms_since_last_tick;
	
	// cerr << " invoke tick with last_tick:" << ms_since_last_tick 
	// 	 << "currentTimer:" << _retransmissionTimer << " _rto:" << _rto << endl;
	
	if (_retransmissionTimer >= _rto)
	{
		if (mapOutstandingSegment.size() == 0)
			return;

		auto it = mapOutstandingSegment.begin();
		_segments_out.push(it->second.tcpSegment);

		// TCPHeader &hdr = it->second.tcpSegment.header();
		// cerr << "hdr info:" << hdr.to_string()
		// 	 << " payload:" << it->second.tcpSegment.payload().size()
		// 	 << " segment_size:" << _segments_out.size()
		// 	 << endl;
			
		if (_notifyWinSize!=0 || _next_seqno==1) {	
			_rto *= 2;		
			//cerr << "tick queue size:" << _segments_out.size() << endl;
			_consecutive_retransmission++;
		}

		_retransmissionTimer = 0;
	}
}

unsigned int TCPSender::consecutive_retransmissions() const { return {_consecutive_retransmission}; }

void TCPSender::send_empty_segment() {
	TCPSegment seg;
	seg.header().seqno = wrap(_next_seqno, _isn);
	_segments_out.push(seg);
}

void TCPSender::send_non_empty_segment(TCPSegment &seg){
    seg.header().seqno = wrap(_next_seqno, _isn);    
    
	// to queue
	_segments_out.push(seg);
	
	// to buffer
	ST_RETRANSSMIT_PACKET stPack;
	stPack.tcpSegment = seg;
	stPack.send_time = 0;
	mapOutstandingSegment.insert(make_pair(_next_seqno, stPack));

	_next_seqno += seg.length_in_sequence_space();
    _byte_in_flight += seg.length_in_sequence_space();
}
