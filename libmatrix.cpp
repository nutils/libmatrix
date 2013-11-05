#include <Tpetra_DefaultPlatform.hpp>
#include <Tpetra_Vector.hpp>
#include <Tpetra_Version.hpp>
#include <Tpetra_CrsMatrix.hpp>

#include <Teuchos_GlobalMPISession.hpp>
#include <Teuchos_oblackholestream.hpp>
#include <Teuchos_DefaultMpiComm.hpp>
#include <Teuchos_oblackholestream.hpp>

#include <mpi.h>


Teuchos::oblackholestream blackHole;

typedef double scalar_t;
typedef int handle_t;
typedef int local_t;
typedef long global_t;

// matching mpi types
const MPI::Datatype MPI_SCALAR = MPI::DOUBLE;
const MPI::Datatype MPI_HANDLE = MPI::INT;
const MPI::Datatype MPI_SIZE = MPI::INT;
const MPI::Datatype MPI_LOCAL = MPI::INT;
const MPI::Datatype MPI_GLOBAL = MPI::LONG;

typedef Kokkos::DefaultNode::DefaultNodeType node_t;
typedef Tpetra::Map<local_t, global_t, node_t> map_t;
typedef Tpetra::Vector<scalar_t, local_t, global_t, node_t> vector_t;
typedef Tpetra::CrsMatrix<scalar_t, local_t, global_t> matrix_t;
typedef Tpetra::CrsGraph<local_t, global_t, node_t> graph_t;

const global_t indexBase = 0;

Teuchos::Array<Teuchos::RCP<vector_t> > VECTORS;
Teuchos::Array<Teuchos::RCP<matrix_t> > MATRICES;
Teuchos::Array<Teuchos::RCP<const map_t> > MAPS;
Teuchos::Array<Teuchos::RCP<const graph_t> > GRAPHS;


/*-------------------------*
 |                         |
 |  MISC HELPER FUNCTIONS  |
 |                         |
 *-------------------------*/


/*
bool verify( bool good, const char *msg, MPI::Intercomm intercomm ) {
  int status = good ? 0 : strlen( msg );
  intercomm.Gather( (void *)(&status), 1, MPI::INT, NULL, 1, MPI::INT, 0 );
  if ( ! good ) {
    intercomm.Send( msg, status, MPI::CHAR, 0, 10 );
  }
  return good;
}
*/


inline std::ostream& out( MPI::Intercomm intercomm ) {
  return
  #ifdef DEBUG
    std::cout << '[' << intercomm.Get_rank() << '/' << intercomm.Get_size() << "] ";
  #else
    blackHole;
  #endif
}


/*-------------------------*
 |                         |
 |      LIBMATRIX API      |
 |                         |
 *-------------------------*/


/* NEW_MAP: create new map
   
     -> broadcast (SIZE) map size
     -> scatter (SIZE) number of items
     -> scatterv (GLOBAL) items
    <-  gather (HANDLE) map id
*/
void new_map( MPI::Intercomm intercomm ) {

  handle_t imap = MAPS.size();

  size_t size, ndofs;
  intercomm.Bcast( (void *)(&size), 1, MPI_SIZE, 0 );
  intercomm.Scatter( NULL, 1, MPI_SIZE, (void *)(&ndofs), 1, MPI_SIZE, 0 );

  out(intercomm) << "creating map #" << imap << " with " << ndofs << '/' << size << " items" << std::endl;

  Teuchos::Array<global_t> elementList( ndofs );
  intercomm.Scatterv( NULL, NULL, NULL, MPI_GLOBAL, (void *)elementList.getRawPtr(), ndofs, MPI_GLOBAL, 0 );

  Teuchos::RCP<node_t> node = Kokkos::DefaultNode::getDefaultNode ();
  Teuchos::RCP<const Teuchos::Comm<int> > comm = Tpetra::DefaultPlatform::getDefaultPlatform().getComm();

  MAPS.push_back( Teuchos::rcp( new map_t( size, elementList, indexBase, comm, node ) ) );

  intercomm.Gather( (void *)(&imap), 1, MPI_HANDLE, NULL, 1, MPI_HANDLE, 0 );
}


/* NEW_VECTOR: create new vector
   
     -> broadcast (HANDLE) map id
    <-  gather (HANDLE) vector id
*/
void new_vector( MPI::Intercomm intercomm ) {

  handle_t ivec = VECTORS.size();

  handle_t imap;
  intercomm.Bcast( (void *)(&imap), 1, MPI_HANDLE, 0 );
  Teuchos::RCP<const map_t> map = MAPS[imap];

  out(intercomm) << "creating vector #" << ivec << " from map #" << imap << std::endl;

  VECTORS.push_back( Teuchos::rcp( new vector_t( map ) ) );

  intercomm.Gather( (void *)(&ivec), 1, MPI_HANDLE, NULL, 1, MPI_HANDLE, 0 );
}


/* NEW_GRAPH: create new graph
   
     -> broadcast (HANDLE) map id
     -> scatterv (SIZE) ncolumns per row
     -> scatterv (GLOBAL) columns (concatenated)
    <-  gather (HANDLE) graph id
*/
void new_graph( MPI::Intercomm intercomm ) {

  handle_t igraph = GRAPHS.size();

  handle_t imap;
  intercomm.Bcast( (void *)(&imap), 1, MPI_HANDLE, 0 );
  Teuchos::RCP<const map_t> map = MAPS[imap];

  size_t nrows = map->getNodeNumElements();
  out(intercomm) << "creating graph #" << igraph << " from map #" << imap << " with " << nrows << " rows" << std::endl;

  Teuchos::ArrayRCP<const size_t> numcols( nrows );
  const size_t *numcols_ptr = numcols.getRawPtr();

  intercomm.Scatterv( NULL, NULL, NULL, MPI_SIZE, (void *)numcols_ptr, nrows, MPI_SIZE, 0 );

  int nitems = 0;
  for ( int irow = 0; irow < nrows; irow++ ) {
    nitems += numcols_ptr[ irow ]; // TODO check if ArrayRCP supports summation
  }

  Teuchos::ArrayRCP<global_t> items( nitems );
  intercomm.Scatterv( NULL, NULL, NULL, MPI_GLOBAL, (void *)items.getRawPtr(), nitems, MPI_GLOBAL, 0 );

  Teuchos::RCP<graph_t> graph = Teuchos::rcp( new graph_t( map, numcols ) );
  size_t offset = 0;
  for ( int irow = 0; irow < nrows; irow++ ) {
    size_t size = numcols_ptr[ irow ];
    graph->insertGlobalIndices( irow, items.view(offset,size) );
    offset += size;
  }
  graph->fillComplete();

  GRAPHS.push_back( graph );

  intercomm.Gather( (void *)(&igraph), 1, MPI_HANDLE, NULL, 1, MPI_HANDLE, 0 );
}


/* ADD_EVEC: add items to vector
   
     -> broadcast (SIZE) rank
   if rank == myrank
     -> recv (HANDLE) vector id
     -> recv (SIZE) number of items
     -> recv (GLOBAL) indices
     -> recv (SCALAR) values
   endif
*/
void add_evec( MPI::Intercomm intercomm ) {

  size_t rank;
  intercomm.Bcast( (void *)(&rank), 1, MPI_SIZE, 0 );

  if ( rank != intercomm.Get_rank() ) {
    return;
  }

  handle_t ivec;
  intercomm.Recv( (void *)(&ivec), 1, MPI_HANDLE, 0, 0 );

  size_t nitems;
  intercomm.Recv( (void *)(&nitems), 1, MPI_SIZE, 0, 0 );

  out(intercomm) << "ivec = " << ivec << ", nitems = " << nitems << std::endl;

  Teuchos::ArrayRCP<global_t> idx( nitems );
  Teuchos::ArrayRCP<scalar_t> data( nitems );

  intercomm.Recv( (void *)idx.getRawPtr(), nitems, MPI_GLOBAL, 0, 0 );
  intercomm.Recv( (void *)data.getRawPtr(), nitems, MPI_SCALAR, 0, 0 );

  Teuchos::RCP<vector_t> vec = VECTORS[ivec];

  for ( int i = 0; i < nitems; i++ ) {
    out(intercomm) << idx[i] << " : " << data[i] << std::endl;
    vec->sumIntoGlobalValue( idx[i], data[i] );
  }

}


/* GET_VECTOR: collect vector over the intercom
  
     -> broadcast (HANDLE) vector id
    <-  gatherv (SCALAR) values
*/
void get_vector( MPI::Intercomm intercomm ) {

  handle_t ivec;
  intercomm.Bcast( (void *)(&ivec), 1, MPI_HANDLE, 0 );
  Teuchos::ArrayRCP<const scalar_t> data = VECTORS[ivec]->getData();

  intercomm.Gatherv( (void *)(data.get()), data.size(), MPI_SCALAR, NULL, NULL, NULL, MPI_SCALAR, 0 );
}


/* NEW_MATRIX: create new matrix
   
     -> broadcast (HANDLE) graph id
    <-  gather (HANDLE) matrix id
*/
void new_matrix( MPI::Intercomm intercomm ) {

  handle_t imat = MATRICES.size();

  handle_t igraph;
  intercomm.Bcast( (void *)(&igraph), 1, MPI_HANDLE, 0 );
  Teuchos::RCP<const graph_t> graph = GRAPHS[igraph];

  out(intercomm) << "creating matrix #" << imat << " from graph #" << igraph << std::endl;

  MATRICES.push_back( Teuchos::rcp( new matrix_t( graph ) ) );

  intercomm.Gather( (void *)(&imat), 1, MPI_HANDLE, NULL, 1, MPI_HANDLE, 0 );
}


/*-------------------------*
 |                         |
 |     MPI SETUP CODE      |
 |                         |
 *-------------------------*/


typedef void ( *funcptr )( MPI::Intercomm );
#define TOKENS new_matrix, new_vector, add_evec, get_vector, new_map, new_graph
funcptr FTABLE[] = { TOKENS };
#define NTOKENS ( sizeof(FTABLE) / sizeof(funcptr) )
#define STR(...) XSTR((__VA_ARGS__))
#define XSTR(s) #s


void eventloop( char *progname ) {

  int argc = 1;
  char **argv = &progname;

  MPI::Init( argc, argv ); 
  MPI::COMM_WORLD.Set_errhandler( MPI::ERRORS_THROW_EXCEPTIONS );
  MPI::Intercomm intercomm = MPI::Comm::Get_parent();

  unsigned char c;
  for ( ;; ) {
    out(intercomm) << "waiting\n";
    intercomm.Bcast( (void *)(&c), 1, MPI::CHAR, 0 );
    out(intercomm) << "received " << (int)c << '\n';
    if ( c >= NTOKENS ) {
      out(intercomm) << "quit\n";
      break;
    }
    FTABLE[c]( intercomm );
  }

  intercomm.Disconnect();
  MPI::Finalize(); 
}


int main( int argc, char *argv[] ) {

  if ( argc == 2 && std::strcmp( argv[1], "info" ) == 0 ) {
    std::cout << "token: enum" << STR(TOKENS) << std::endl;
    std::cout << "local: int" << (sizeof(local_t) << 3) << std::endl;
    std::cout << "global: int" << (sizeof(global_t) << 3) << std::endl;
    std::cout << "size: int" << (sizeof(size_t) << 3) << std::endl;
    std::cout << "handle: int" << (sizeof(handle_t) << 3) << std::endl;
    std::cout << "scalar: float" << (sizeof(scalar_t) << 3) << std::endl;
  }
  else if ( argc == 2 && std::strcmp( argv[1], "eventloop" ) == 0 ) {
    eventloop( argv[0] );
  }
  else {
    std::cout << "syntax: " << argv[0] << " info|eventloop" << std::endl;
    return 1;
  }
  return 0;
}


// vim:foldmethod=syntax
