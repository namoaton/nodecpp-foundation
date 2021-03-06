/* -------------------------------------------------------------------------------
* Copyright (c) 2020, OLogN Technologies AG
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions are met:
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in the
*       documentation and/or other materials provided with the distribution.
*     * Neither the name of the OLogN Technologies AG nor the
*       names of its contributors may be used to endorse or promote products
*       derived from this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL OLogN Technologies AG BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
* -------------------------------------------------------------------------------*/

#ifndef INTERNAL_MSG_H
#define INTERNAL_MSG_H

#include "foundation.h"
#include "page_allocator.h"
#ifdef NODECPP_MSVC
#include <intrin.h>
#endif

namespace nodecpp::platform::internal_msg { 

	constexpr size_t pageSize = 0x1000;
	constexpr size_t pageSizeExp = 12;
	static_assert( (1<<pageSizeExp) == pageSize );

	struct PagePtrWrapper
	{
	private:
		uint8_t* ptr;
	public:
		PagePtrWrapper() {init();}
		PagePtrWrapper( void* page ) {init( page );}
		PagePtrWrapper( const PagePtrWrapper& other ) = default;
		PagePtrWrapper& operator =( const PagePtrWrapper& other ) = default;
		PagePtrWrapper( PagePtrWrapper&& other ) = default;
		PagePtrWrapper& operator =( PagePtrWrapper&& other ) = default;
		~PagePtrWrapper() {}
		uint8_t* page() { return ptr; }
		void init() {ptr = 0;}
		void init( void* page ) {ptr = reinterpret_cast<uint8_t*>(page); }
	};

	using PagePointer = PagePtrWrapper;
//	using PagePointer = page_ptr_and_data;

	class MallocPageProvider // used just for interface purposes
	{
	public:
		static PagePointer acquirePage() { return PagePointer( ::malloc( pageSize ) ); }
		static void releasePage( PagePointer page ) { ::free( page.page() ); }
	};

	class InternalMsg
	{
		PagePointer implAcquirePageWrapper()
		{
			return MallocPageProvider::acquirePage();
			//return globalPagePool.acquirePage();
		}
		void implReleasePageWrapper( PagePointer page )
		{
			MallocPageProvider::releasePage( page );
			//globalPagePool.releasePage( page );
		}
		struct IndexPageHeader
		{
			PagePointer next_;
			size_t usedCnt;
			PagePointer* pages() { return reinterpret_cast<PagePointer*>(this+1); }
			IndexPageHeader() { init(); }
			IndexPageHeader( const IndexPageHeader& ) = delete;
			IndexPageHeader& operator = ( const IndexPageHeader& ) = delete;
			IndexPageHeader( IndexPageHeader&& other )
			{
				usedCnt = other.usedCnt;
				other.usedCnt = 0;
				next_ = other.next_;
				other.next_.init();
			}
			IndexPageHeader& operator = ( IndexPageHeader&& other )
			{
				usedCnt = other.usedCnt;
				other.usedCnt = 0;
				next_ = other.next_;
				other.next_.init();
				return *this;
			}
			void init() { next_.init(); usedCnt = 0; pages()[0].init();}
			void init(PagePointer page) { next_.init(); usedCnt = 1; pages()[0] = page;}
			void setNext( PagePointer n ) { next_ = n; }
			IndexPageHeader* next() { return reinterpret_cast<IndexPageHeader*>( next_.page() ); }
		};
		static constexpr size_t maxAddressedByPage = ( pageSize - sizeof( IndexPageHeader ) ) / sizeof( uint8_t*);

		static constexpr size_t localStorageSize = 4;
		struct FirstHeader : public IndexPageHeader
		{
			PagePointer firstPages[ localStorageSize ];
			FirstHeader() : IndexPageHeader() {}
			FirstHeader( const FirstHeader& ) = delete;
			FirstHeader& operator = ( const FirstHeader& ) = delete;
			FirstHeader( FirstHeader&& other ) : IndexPageHeader( std::move( other ) )
			{
				for ( size_t i=0; i<localStorageSize;++i )
				{
					firstPages[i] = other.firstPages[i];
					other.firstPages[i].init();
				}
			}
			FirstHeader& operator = ( FirstHeader&& other )
			{
				IndexPageHeader::operator = ( std::move( other ) );
				for ( size_t i=0; i<localStorageSize;++i )
				{
					firstPages[i] = other.firstPages[i];
					other.firstPages[i].init();
				}
				return *this;
			}
		};
		static_assert( sizeof( IndexPageHeader ) + sizeof( PagePointer ) * localStorageSize == sizeof(FirstHeader) );
		FirstHeader firstHeader;

		size_t pageCnt = 0; // payload pages (that is, not include index pages)
		PagePointer lip;
		IndexPageHeader* lastIndexPage() { return reinterpret_cast<IndexPageHeader*>( lip.page() ); }
		PagePointer currentPage;
		size_t totalSz = 0;

		size_t offsetInCurrentPage() { return totalSz & ( pageSize - 1 ); }
		size_t remainingSizeInCurrentPage() { return pageSize - offsetInCurrentPage(); }
		void implReleaseAllPages()
		{
			size_t i;
			for ( i=0; i<localStorageSize && i<pageCnt; ++i )
				implReleasePageWrapper( firstHeader.firstPages[i] );
			pageCnt -= i;
			if ( pageCnt )
			{
				NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, firstHeader.next() != nullptr );
				while ( firstHeader.next() != nullptr )
				{
					NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, pageCnt > localStorageSize || firstHeader.next()->next() == nullptr );
					NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, firstHeader.next()->usedCnt <= pageCnt );
					for ( size_t i=0; i<firstHeader.next()->usedCnt; ++i )
						implReleasePageWrapper( firstHeader.next()->pages()[i] );
					pageCnt -= firstHeader.next()->usedCnt;
					PagePointer page = firstHeader.next_;
					firstHeader.next_ = firstHeader.next()->next_;
					implReleasePageWrapper( page );
				}
			}
		}

		void implAddPage() // TODO: revise
		{
			NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, currentPage.page() == nullptr );
			NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, offsetInCurrentPage() == 0 );
			if ( pageCnt < localStorageSize )
			{
				NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, firstHeader.next() == nullptr );
				NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, lip.page() == nullptr );
				firstHeader.firstPages[pageCnt] = implAcquirePageWrapper();
				currentPage = firstHeader.firstPages[pageCnt];
				++(firstHeader.usedCnt);
			}
			else if ( lastIndexPage() == nullptr )
			{
				NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, firstHeader.next() == nullptr );
				lip = implAcquirePageWrapper();
				firstHeader.setNext(lip);
				currentPage = implAcquirePageWrapper();
				lastIndexPage()->init( currentPage );
//				lip = currentPage;
			}
			else if ( lastIndexPage()->usedCnt == maxAddressedByPage )
			{
				NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, firstHeader.next() != nullptr );
				PagePointer nextip_ = implAcquirePageWrapper();
				IndexPageHeader* nextip = reinterpret_cast<IndexPageHeader*>(nextip_.page());
				currentPage = implAcquirePageWrapper();
				nextip->init( currentPage );
				lastIndexPage()->next_ = nextip_;
				lip = nextip_;
			}
			else
			{
				NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, firstHeader.next() != nullptr );
				NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, lastIndexPage()->usedCnt < maxAddressedByPage );
				currentPage = implAcquirePageWrapper();
				lastIndexPage()->pages()[lastIndexPage()->usedCnt] = currentPage;
				++(lastIndexPage()->usedCnt);
			}
			++pageCnt;
		}

	public:
		class ReadIter
		{
			friend class InternalMsg;
			IndexPageHeader* ip;
			const uint8_t* page;
			size_t totalSz;
			size_t sizeRemainingInBlock;
			size_t idxInIndexPage;
			ReadIter( IndexPageHeader* ip_, const uint8_t* page_, size_t sz ) : ip( ip_ ), page( page_ ), totalSz( sz )
			{
				sizeRemainingInBlock = sz <= pageSize ? sz : pageSize;
				idxInIndexPage = 0;
			}
		public:
			size_t availableSize() {return sizeRemainingInBlock;}
			const uint8_t* read( size_t sz )
			{
				NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, sz <= sizeRemainingInBlock );
				NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, ip != nullptr );
				sizeRemainingInBlock -= sz;
				totalSz -= sz;
				const uint8_t* ret = page;
				if ( totalSz && sizeRemainingInBlock == 0 )
				{
					if ( idxInIndexPage + 1 >= ip->usedCnt )
					{
						ip = ip->next();
						idxInIndexPage = 0;
						page = ip->pages()[idxInIndexPage].page();
					}
					else
						page = ip->pages()[++idxInIndexPage].page();
					sizeRemainingInBlock = totalSz <= pageSize ? totalSz : pageSize;
				}
				else
					page += sz;
				return ret;
			}
		};

	public:
		InternalMsg() { firstHeader.init(); memset( firstHeader.firstPages, 0, sizeof( firstHeader.firstPages ) ); }
		InternalMsg( const InternalMsg& ) = delete;
		InternalMsg& operator = ( const InternalMsg& ) = delete;
		InternalMsg( InternalMsg&& other )
		{
			firstHeader = std::move( other.firstHeader );
			pageCnt = other.pageCnt;
			other.pageCnt = 0;
			lip = other.lip;
			other.lip.init();
			currentPage = other.currentPage;
			other.currentPage.init();
			totalSz = other.totalSz;
			other.totalSz = 0;
		}
		InternalMsg& operator = ( InternalMsg&& other )
		{
			if ( this == &other ) return *this;
			firstHeader = std::move( other.firstHeader );
			pageCnt = other.pageCnt;
			other.pageCnt = 0;
			lip = other.lip;
			other.lip.init();
			currentPage = other.currentPage;
			other.currentPage.init();
			totalSz = other.totalSz;
			other.totalSz = 0;
			return *this;
		}
		void append( const void* buff_, size_t sz )
		{
			const uint8_t* buff = reinterpret_cast<const uint8_t*>(buff_);
			while( sz != 0 )
			{
				if ( currentPage.page() == nullptr )
				{
					implAddPage();
					NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, currentPage.page() != nullptr );
				}
				size_t remainingInPage = remainingSizeInCurrentPage();
				if ( sz <= remainingInPage )
				{
					memcpy( currentPage.page() + offsetInCurrentPage(), buff, sz );
					totalSz += sz;
					if( sz == remainingInPage )
					{
						NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, offsetInCurrentPage() == 0 );
						currentPage.init();
					}
					break;
				}
				else
				{
					memcpy( currentPage.page() + offsetInCurrentPage(), buff, remainingInPage );
					sz -= remainingInPage;
					buff += remainingInPage;
					totalSz += remainingInPage;
					NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, offsetInCurrentPage() == 0 );
					currentPage.init();
				}
			}
		}

		ReadIter getReadIter()
		{
			return ReadIter( &firstHeader, firstHeader.pages()[0].page(), totalSz );
		}

		void clear() 
		{
			implReleaseAllPages();
			NODECPP_ASSERT( nodecpp::foundation::module_id, nodecpp::assert::AssertLevel::pedantic, pageCnt == 0 );
			firstHeader.next_.init();
			lip.init();
			currentPage.init();
			totalSz = 0;
		}

		size_t size() { return totalSz; }
		~InternalMsg() { implReleaseAllPages(); }
	};

} // nodecpp


#endif // INTERNAL_MSG_H